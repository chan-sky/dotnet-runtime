// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                           Importer                                        XX
XX                                                                           XX
XX   Imports the given method and converts it to semantic trees              XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#include "corexcep.h"

/*****************************************************************************
 *
 *  Pushes the given tree on the stack.
 */

void Compiler::impPushOnStack(GenTree* tree, typeInfo ti)
{
    /* Check for overflow. If inlining, we may be using a bigger stack */

    if ((stackState.esStackDepth >= info.compMaxStack) &&
        (stackState.esStackDepth >= impStkSize || !compCurBB->HasFlag(BBF_IMPORTED)))
    {
        BADCODE("stack overflow");
    }

    stackState.esStack[stackState.esStackDepth].seTypeInfo = ti;
    stackState.esStack[stackState.esStackDepth++].val      = tree;

    if (tree->TypeIs(TYP_LONG))
    {
        compLongUsed = true;
    }
    else if (tree->TypeIs(TYP_FLOAT) || tree->TypeIs(TYP_DOUBLE))
    {
        compFloatingPointUsed = true;
    }
}

// helper function that will tell us if the IL instruction at the addr passed
// by param consumes an address at the top of the stack. We use it to save
// us lvAddrTaken
bool Compiler::impILConsumesAddr(const BYTE* codeAddr, const BYTE* codeEndp)
{
    OPCODE opcode = impGetNonPrefixOpcode(codeAddr, codeEndp);
    switch (opcode)
    {
        case CEE_LDFLD:
        {
            return true;
        }

        default:
            break;
    }

    return false;
}

void Compiler::impResolveToken(const BYTE* addr, CORINFO_RESOLVED_TOKEN* pResolvedToken, CorInfoTokenKind kind)
{
    pResolvedToken->tokenContext = impTokenLookupContextHandle;
    pResolvedToken->tokenScope   = info.compScopeHnd;
    pResolvedToken->token        = getU4LittleEndian(addr);
    pResolvedToken->tokenType    = kind;

    info.compCompHnd->resolveToken(pResolvedToken);
}

//------------------------------------------------------------------------
// impPopStack: Pop one tree from the stack.
//
// Returns:
//   The stack entry for the popped tree.
//
StackEntry Compiler::impPopStack()
{
    if (stackState.esStackDepth == 0)
    {
        BADCODE("stack underflow");
    }

    return stackState.esStack[--stackState.esStackDepth];
}

//------------------------------------------------------------------------
// impPopStack: Pop a variable number of trees from the stack.
//
// Arguments:
//   n - The number of trees to pop.
//
void Compiler::impPopStack(unsigned n)
{
    if (stackState.esStackDepth < n)
    {
        BADCODE("stack underflow");
    }

    stackState.esStackDepth -= n;
}

/*****************************************************************************
 *
 *  Peep at n'th (0-based) tree on the top of the stack.
 */

StackEntry& Compiler::impStackTop(unsigned n)
{
    if (stackState.esStackDepth <= n)
    {
        BADCODE("stack underflow");
    }

    return stackState.esStack[stackState.esStackDepth - n - 1];
}

unsigned Compiler::impStackHeight()
{
    return stackState.esStackDepth;
}

/*****************************************************************************
 *  Some of the trees are spilled specially. While unspilling them, or
 *  making a copy, these need to be handled specially. The function
 *  enumerates the operators possible after spilling.
 */

#ifdef DEBUG // only used in asserts
static bool impValidSpilledStackEntry(GenTree* tree)
{
    if (tree->OperIs(GT_LCL_VAR))
    {
        return true;
    }

    if (tree->OperIsConst())
    {
        return true;
    }

    return false;
}
#endif

/*****************************************************************************
 *
 *  The following logic is used to save/restore stack contents.
 *  If 'copy' is true, then we make a copy of the trees on the stack. These
 *  have to all be cloneable/spilled values.
 */

void Compiler::impSaveStackState(SavedStack* savePtr, bool copy)
{
    savePtr->ssDepth = stackState.esStackDepth;

    if (stackState.esStackDepth)
    {
        savePtr->ssTrees = new (this, CMK_ImpStack) StackEntry[stackState.esStackDepth];
        size_t saveSize  = stackState.esStackDepth * sizeof(*savePtr->ssTrees);

        if (copy)
        {
            StackEntry* table = savePtr->ssTrees;

            /* Make a fresh copy of all the stack entries */

            for (unsigned level = 0; level < stackState.esStackDepth; level++, table++)
            {
                table->seTypeInfo = stackState.esStack[level].seTypeInfo;
                GenTree* tree     = stackState.esStack[level].val;

                assert(impValidSpilledStackEntry(tree));

                switch (tree->gtOper)
                {
                    case GT_CNS_INT:
                    case GT_CNS_LNG:
                    case GT_CNS_DBL:
                    case GT_CNS_STR:
#if defined(FEATURE_SIMD)
                    case GT_CNS_VEC:
#endif // FEATURE_SIMD
#if defined(FEATURE_MASKED_HW_INTRINSICS)
                    case GT_CNS_MSK:
#endif // FEATURE_MASKED_HW_INTRINSICS
                    case GT_LCL_VAR:
                        table->val = gtCloneExpr(tree);
                        break;

                    default:
                        assert(!"Bad oper - Not covered by impValidSpilledStackEntry()");
                        break;
                }
            }
        }
        else
        {
            memcpy(savePtr->ssTrees, stackState.esStack, saveSize);
        }
    }
}

void Compiler::impRestoreStackState(SavedStack* savePtr)
{
    stackState.esStackDepth = savePtr->ssDepth;

    if (stackState.esStackDepth)
    {
        memcpy(stackState.esStack, savePtr->ssTrees, stackState.esStackDepth * sizeof(*stackState.esStack));
    }
}

//------------------------------------------------------------------------
// impBeginTreeList: Get the tree list started for a new basic block.
//
void Compiler::impBeginTreeList()
{
    assert(impStmtList == nullptr && impLastStmt == nullptr);
}

/*****************************************************************************
 *
 *  Store the given start and end stmt in the given basic block. This is
 *  mostly called by impEndTreeList(BasicBlock *block). It is called
 *  directly only for handling CEE_LEAVEs out of finally-protected try's.
 */

void Compiler::impEndTreeList(BasicBlock* block, Statement* firstStmt, Statement* lastStmt)
{
    /* Make the list circular, so that we can easily walk it backwards */

    firstStmt->SetPrevStmt(lastStmt);

    /* Store the tree list in the basic block */

    block->bbStmtList = firstStmt;

    /* The block should not already be marked as imported */
    assert(!block->HasFlag(BBF_IMPORTED));

    block->SetFlags(BBF_IMPORTED);
}

void Compiler::impEndTreeList(BasicBlock* block)
{
    if (impStmtList == nullptr)
    {
        // The block should not already be marked as imported.
        assert(!block->HasFlag(BBF_IMPORTED));

        // Empty block. Just mark it as imported.
        block->SetFlags(BBF_IMPORTED);
    }
    else
    {
        impEndTreeList(block, impStmtList, impLastStmt);
    }

#ifdef DEBUG
    if (impLastILoffsStmt != nullptr)
    {
        impLastILoffsStmt->SetLastILOffset(compIsForInlining() ? BAD_IL_OFFSET : impCurOpcOffs);
        impLastILoffsStmt = nullptr;
    }
#endif
    impStmtList = impLastStmt = nullptr;
}

/*****************************************************************************
 *
 *  Check that storing the given tree doesnt mess up the semantic order. Note
 *  that this has only limited value as we can only check [0..chkLevel).
 */

void Compiler::impAppendStmtCheck(Statement* stmt, unsigned chkLevel)
{
#ifndef DEBUG
    return;
#else

    if (chkLevel == CHECK_SPILL_ALL)
    {
        chkLevel = stackState.esStackDepth;
    }

    if (stackState.esStackDepth == 0 || chkLevel == 0 || chkLevel == CHECK_SPILL_NONE)
    {
        return;
    }

    GenTree* tree = stmt->GetRootNode();

    // Calls can only be appended if there are no GTF_GLOB_EFFECT on the stack

    if (tree->gtFlags & GTF_CALL)
    {
        for (unsigned level = 0; level < chkLevel; level++)
        {
            assert((stackState.esStack[level].val->gtFlags & GTF_GLOB_EFFECT) == 0);
        }
    }

    if (tree->OperIsStore())
    {
        // For a store to a local variable, all references of that variable have to be spilled.
        // If it is aliased, all calls and indirect accesses have to be spilled.
        if (tree->OperIsLocalStore())
        {
            unsigned lclNum = tree->AsLclVarCommon()->GetLclNum();
            for (unsigned level = 0; level < chkLevel; level++)
            {
                GenTree* stkTree = stackState.esStack[level].val;
                assert(!gtHasRef(stkTree, lclNum) || impIsInvariant(stkTree));
                assert(!lvaTable[lclNum].IsAddressExposed() || ((stkTree->gtFlags & GTF_SIDE_EFFECT) == 0));
            }
        }
        // If the access may be to global memory, all side effects have to be spilled.
        else if ((tree->gtFlags & GTF_GLOB_REF) != 0)
        {
            for (unsigned level = 0; level < chkLevel; level++)
            {
                assert((stackState.esStack[level].val->gtFlags & GTF_GLOB_REF) == 0);
            }
        }
    }
#endif
}

//------------------------------------------------------------------------
// impAppendStmt: Append the given statement to the current block's tree list.
//
//
// Arguments:
//    stmt                   - The statement to add.
//    chkLevel               - [0..chkLevel) is the portion of the stack which we will check
//                             for interference with stmt and spilled if needed.
//    checkConsumedDebugInfo - Whether to check for consumption of impCurStmtDI. impCurStmtDI
//                             marks the debug info of the current boundary and is set when we
//                             start importing IL at that boundary. If this parameter is true,
//                             then the function checks if 'stmt' has been associated with the
//                             current boundary, and if so, clears it so that we do not attach
//                             it to more upcoming statements.
//
void Compiler::impAppendStmt(Statement* stmt, unsigned chkLevel, bool checkConsumedDebugInfo)
{
    if (chkLevel == CHECK_SPILL_ALL)
    {
        chkLevel = stackState.esStackDepth;
    }

    if ((chkLevel != 0) && (chkLevel != CHECK_SPILL_NONE))
    {
        assert(chkLevel <= stackState.esStackDepth);

        // If the statement being appended has any side-effects, check the stack to see if anything
        // needs to be spilled to preserve correct ordering.
        //
        GenTree*     expr  = stmt->GetRootNode();
        GenTreeFlags flags = expr->gtFlags & GTF_GLOB_EFFECT;

        // Stores to unaliased locals require special handling. Here, we look for trees that
        // can modify them and spill the references. In doing so, we make two assumptions:
        //
        // 1. All locals which can be modified indirectly are marked as address-exposed or with
        //    "lvHasLdAddrOp" -- we will rely on "impSpillSideEffects(spillGlobEffects: true)"
        //    below to spill them.
        // 2. Trees that assign to unaliased locals are always top-level (this avoids having to
        //    walk down the tree here), and are a subset of what is recognized here.
        //
        // If any of the above are violated (say for some temps), the relevant code must spill
        // things manually.
        //
        LclVarDsc* dstVarDsc = nullptr;
        if (expr->OperIsLocalStore())
        {
            dstVarDsc = lvaGetDesc(expr->AsLclVarCommon());
        }
        else if (expr->OperIs(GT_CALL, GT_RET_EXPR)) // The special case of calls with return buffers.
        {
            GenTree* call = expr->OperIs(GT_RET_EXPR) ? expr->AsRetExpr()->gtInlineCandidate : expr;

            if (call->TypeIs(TYP_VOID) && call->AsCall()->ShouldHaveRetBufArg())
            {
                GenTree* retBuf;
                if (call->AsCall()->ShouldHaveRetBufArg())
                {
                    assert(call->AsCall()->gtArgs.HasRetBuffer());
                    retBuf = call->AsCall()->gtArgs.GetRetBufferArg()->GetNode();
                }
                else
                {
                    assert(!call->AsCall()->gtArgs.HasThisPointer());
                    retBuf = call->AsCall()->gtArgs.GetArgByIndex(0)->GetNode();
                }

                assert(retBuf->TypeIs(TYP_I_IMPL, TYP_BYREF));

                if (retBuf->OperIs(GT_LCL_ADDR))
                {
                    dstVarDsc = lvaGetDesc(retBuf->AsLclVarCommon());
                }
            }
        }

        // In the case of GT_RET_EXPR any subsequent spills will appear in the wrong place -- after
        // the call. We need to move them to before the call
        //
        Statement* lastStmt = impLastStmt;

        if ((dstVarDsc != nullptr) && !dstVarDsc->IsAddressExposed() && !dstVarDsc->lvHasLdAddrOp)
        {
            impSpillLclRefs(lvaGetLclNum(dstVarDsc), chkLevel);

            if (expr->OperIsLocalStore())
            {
                // For stores, limit the checking to what the value could modify/interfere with.
                GenTree* value = expr->AsLclVarCommon()->Data();
                flags          = value->gtFlags & GTF_GLOB_EFFECT;

                // We don't mark indirections off of "aliased" locals with GLOB_REF, but they must still be
                // considered as such in the interference checking.
                if (((flags & GTF_GLOB_REF) == 0) && !impIsAddressInLocal(value) && gtHasLocalsWithAddrOp(value))
                {
                    flags |= GTF_GLOB_REF;
                }
            }
        }

        if (flags != 0)
        {
            impSpillSideEffects((flags & (GTF_ASG | GTF_CALL)) != 0, chkLevel DEBUGARG("impAppendStmt"));
        }
        else
        {
            impSpillSpecialSideEff();
        }

        if ((lastStmt != impLastStmt) && expr->OperIs(GT_RET_EXPR))
        {
            GenTree* const call = expr->AsRetExpr()->gtInlineCandidate;
            JITDUMP("\nimpAppendStmt: after sinking a local struct store into inline candidate [%06u], we need to "
                    "reorder subsequent spills.\n",
                    dspTreeID(call));

            // Move all newly appended statements to just before the call's statement.
            // First, find the statement containing the call.
            //
            Statement* insertBeforeStmt = lastStmt;

            while (insertBeforeStmt->GetRootNode() != call)
            {
                assert(insertBeforeStmt != impStmtList);
                insertBeforeStmt = insertBeforeStmt->GetPrevStmt();
            }

            Statement* movingStmt = lastStmt->GetNextStmt();

            JITDUMP("Moving " FMT_STMT " through " FMT_STMT " before " FMT_STMT "\n", movingStmt->GetID(),
                    impLastStmt->GetID(), insertBeforeStmt->GetID());

            // We move these backwards, so must keep moving the insert
            // point to keep them in order.
            //
            while (impLastStmt != lastStmt)
            {
                Statement* movingStmt = impExtractLastStmt();
                impInsertStmtBefore(movingStmt, insertBeforeStmt);
                insertBeforeStmt = movingStmt;
            }
        }
    }

    impAppendStmtCheck(stmt, chkLevel);

    impAppendStmt(stmt);

#ifdef FEATURE_SIMD
    impMarkContiguousSIMDFieldStores(stmt);
#endif

    // Once we set the current offset as debug info in an appended tree, we are
    // ready to report the following offsets. Note that we need to compare
    // offsets here instead of debug info, since we do not set the "is call"
    // bit in impCurStmtDI.

    if (checkConsumedDebugInfo &&
        (impLastStmt->GetDebugInfo().GetLocation().GetOffset() == impCurStmtDI.GetLocation().GetOffset()))
    {
        impCurStmtOffsSet(BAD_IL_OFFSET);
    }

#ifdef DEBUG
    if (impLastILoffsStmt == nullptr)
    {
        impLastILoffsStmt = stmt;
    }

    if (verbose)
    {
        printf("\n\n");
        gtDispStmt(stmt);
    }
#endif
}

//------------------------------------------------------------------------
// impAppendStmt: Add the statement to the current stmts list.
//
// Arguments:
//    stmt - the statement to add.
//
void Compiler::impAppendStmt(Statement* stmt)
{
    if (impStmtList == nullptr)
    {
        // The stmt is the first in the list.
        impStmtList = stmt;
    }
    else
    {
        // Append the expression statement to the existing list.
        impLastStmt->SetNextStmt(stmt);
        stmt->SetPrevStmt(impLastStmt);
    }
    impLastStmt = stmt;
}

//------------------------------------------------------------------------
// impExtractLastStmt: Extract the last statement from the current stmts list.
//
// Return Value:
//    The extracted statement.
//
// Notes:
//    It assumes that the stmt will be reinserted later.
//
Statement* Compiler::impExtractLastStmt()
{
    assert(impLastStmt != nullptr);

    Statement* stmt = impLastStmt;
    impLastStmt     = impLastStmt->GetPrevStmt();
    if (impLastStmt == nullptr)
    {
        impStmtList = nullptr;
    }
    return stmt;
}

//-------------------------------------------------------------------------
// impInsertStmtBefore: Insert the given "stmt" before "stmtBefore".
//
// Arguments:
//    stmt       - a statement to insert;
//    stmtBefore - an insertion point to insert "stmt" before.
//
void Compiler::impInsertStmtBefore(Statement* stmt, Statement* stmtBefore)
{
    assert(stmt != nullptr);
    assert(stmtBefore != nullptr);

    if (stmtBefore == impStmtList)
    {
        impStmtList = stmt;
    }
    else
    {
        Statement* stmtPrev = stmtBefore->GetPrevStmt();
        stmt->SetPrevStmt(stmtPrev);
        stmtPrev->SetNextStmt(stmt);
    }
    stmt->SetNextStmt(stmtBefore);
    stmtBefore->SetPrevStmt(stmt);
}

//------------------------------------------------------------------------
// impAppendTree: Append the given expression tree to the current block's tree list.
//
//
// Arguments:
//    tree                   - The tree that will be the root of the newly created statement.
//    chkLevel               - [0..chkLevel) is the portion of the stack which we will check
//                             for interference with stmt and spill if needed.
//    di                     - Debug information to associate with the statement.
//    checkConsumedDebugInfo - Whether to check for consumption of impCurStmtDI. impCurStmtDI
//                             marks the debug info of the current boundary and is set when we
//                             start importing IL at that boundary. If this parameter is true,
//                             then the function checks if 'stmt' has been associated with the
//                             current boundary, and if so, clears it so that we do not attach
//                             it to more upcoming statements.
//
// Return value:
//   The newly created statement.
//
Statement* Compiler::impAppendTree(GenTree* tree, unsigned chkLevel, const DebugInfo& di, bool checkConsumedDebugInfo)
{
    assert(tree);

    /* Allocate an 'expression statement' node */

    Statement* stmt = gtNewStmt(tree, di);

    /* Append the statement to the current block's stmt list */

    impAppendStmt(stmt, chkLevel, checkConsumedDebugInfo);

    return stmt;
}

/*****************************************************************************
 *
 *  Append a store of the given value to a temp to the current tree list.
 *  curLevel is the stack level for which the spill to the temp is being done.
 */

void Compiler::impStoreToTemp(unsigned         lclNum,
                              GenTree*         val,
                              unsigned         curLevel,
                              Statement**      pAfterStmt, /* = NULL */
                              const DebugInfo& di,         /* = DebugInfo() */
                              BasicBlock*      block       /* = NULL */
)
{
    GenTree* store = gtNewTempStore(lclNum, val, curLevel, pAfterStmt, di, block);

    if (!store->IsNothingNode())
    {
        if (pAfterStmt)
        {
            Statement* storeStmt = gtNewStmt(store, di);
            fgInsertStmtAfter(block, *pAfterStmt, storeStmt);
            *pAfterStmt = storeStmt;
        }
        else
        {
            impAppendTree(store, curLevel, impCurStmtDI);
        }
    }
}

static bool TypeIs(var_types type1, var_types type2)
{
    return type1 == type2;
}

// Check if type1 matches any type from the list.
template <typename... T>
static bool TypeIs(var_types type1, var_types type2, T... rest)
{
    return TypeIs(type1, type2) || TypeIs(type1, rest...);
}

//------------------------------------------------------------------------
// impCheckImplicitArgumentCoercion: check that the node's type is compatible with
//   the signature's type using ECMA implicit argument coercion table.
//
// Arguments:
//    sigType  - the type in the call signature;
//    nodeType - the node type.
//
// Return Value:
//    true if they are compatible, false otherwise.
//
// Notes:
//   - it is currently allowing byref->long passing, should be fixed in VM;
//   - it can't check long -> native int case on 64-bit platforms,
//      so the behavior is different depending on the target bitness.
//
bool Compiler::impCheckImplicitArgumentCoercion(var_types sigType, var_types nodeType)
{
    if (sigType == nodeType)
    {
        return true;
    }

    if (TypeIs(sigType, TYP_UBYTE, TYP_BYTE, TYP_USHORT, TYP_SHORT, TYP_UINT, TYP_INT))
    {
        if (TypeIs(nodeType, TYP_UBYTE, TYP_BYTE, TYP_USHORT, TYP_SHORT, TYP_UINT, TYP_INT, TYP_I_IMPL))
        {
            return true;
        }
    }
    else if (TypeIs(sigType, TYP_ULONG, TYP_LONG))
    {
        if (TypeIs(nodeType, TYP_LONG))
        {
            return true;
        }
    }
    else if (TypeIs(sigType, TYP_FLOAT, TYP_DOUBLE))
    {
        if (TypeIs(nodeType, TYP_FLOAT, TYP_DOUBLE))
        {
            return true;
        }
    }
    else if (TypeIs(sigType, TYP_BYREF))
    {
        if (TypeIs(nodeType, TYP_I_IMPL))
        {
            return true;
        }

        // This condition tolerates such IL:
        // ;  V00 this              ref  this class-hnd
        // ldarg.0
        // call(byref)
        if (TypeIs(nodeType, TYP_REF))
        {
            return true;
        }
    }
    else if (varTypeIsStruct(sigType))
    {
        if (varTypeIsStruct(nodeType))
        {
            return true;
        }
    }

    // This condition should not be under `else` because `TYP_I_IMPL`
    // intersects with `TYP_LONG` or `TYP_INT`.
    if (TypeIs(sigType, TYP_I_IMPL, TYP_U_IMPL))
    {
        // Note that it allows `ldc.i8 1; call(nint)` on 64-bit platforms,
        // but we can't distinguish `nint` from `long` there.
        if (TypeIs(nodeType, TYP_I_IMPL, TYP_U_IMPL, TYP_INT, TYP_UINT))
        {
            return true;
        }

        // It tolerates IL that ECMA does not allow but that is commonly used.
        // Example:
        //   V02 loc1           struct <RTL_OSVERSIONINFOEX, 32>
        //   ldloca.s     0x2
        //   call(native int)
        if (TypeIs(nodeType, TYP_BYREF))
        {
            return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------
// impStoreStruct: Import a struct store.
//
// Arguments:
//    store        - the store
//    curLevel     - stack level for which a spill may be being done
//    pAfterStmt   - statement to insert any additional statements after
//    di           - debug info for new statements
//    block        - block to insert any additional statements in
//
// Return Value:
//    The tree that should be appended to the statement list that represents the store.
//
// Notes:
//    Temp stores may be appended to impStmtList if spilling is necessary.
//
GenTree* Compiler::impStoreStruct(GenTree*         store,
                                  unsigned         curLevel,
                                  Statement**      pAfterStmt, /* = nullptr */
                                  const DebugInfo& di,         /* = DebugInfo() */
                                  BasicBlock*      block       /* = nullptr */
)
{
    assert(varTypeIsStruct(store) && store->OperIsStore());

    GenTree* src = store->Data();

    assert(store->TypeGet() == src->TypeGet());
    if (store->TypeIs(TYP_STRUCT))
    {
        assert(ClassLayout::AreCompatible(store->GetLayout(this), src->GetLayout(this)));
    }

    DebugInfo usedDI = di;
    if (!usedDI.IsValid())
    {
        usedDI = impCurStmtDI;
    }

    if (src->IsCall())
    {
        GenTreeCall* srcCall = src->AsCall();
        if (srcCall->ShouldHaveRetBufArg())
        {
            // Case of call returning a struct via hidden retbuf arg.
            // Some calls have an "out buffer" that is not actually a ret buff
            // in the ABI sense. We take the path here for those but it should
            // not be marked as the ret buff arg since it always follow the
            // normal ABI for parameters.
            WellKnownArg wellKnownArgType =
                srcCall->ShouldHaveRetBufArg() ? WellKnownArg::RetBuffer : WellKnownArg::None;

            // TODO-Bug?: verify if flags matter here
            GenTreeFlags indirFlags = GTF_EMPTY;
            GenTree*     destAddr   = impGetNodeAddr(store, CHECK_SPILL_ALL, &indirFlags);

            if (!impIsLegalRetBuf(destAddr, srcCall))
            {
                unsigned tmp = lvaGrabTemp(false DEBUGARG("stack copy for value returned via return buffer"));
                lvaSetStruct(tmp, srcCall->gtRetClsHnd, false);

                GenTree* spilledCall = gtNewStoreLclVarNode(tmp, srcCall);
                spilledCall          = impStoreStruct(spilledCall, curLevel, pAfterStmt, di, block);
                store->Data()        = gtNewOperNode(GT_COMMA, store->TypeGet(), spilledCall,
                                                     gtNewLclvNode(tmp, lvaGetDesc(tmp)->TypeGet()));

                return impStoreStruct(store, curLevel, pAfterStmt, di, block);
            }

            NewCallArg newArg = NewCallArg::Primitive(destAddr).WellKnown(wellKnownArgType);

            if (destAddr->OperIs(GT_LCL_ADDR))
            {
                lvaSetVarDoNotEnregister(destAddr->AsLclVarCommon()->GetLclNum()
                                             DEBUGARG(DoNotEnregisterReason::HiddenBufferStructArg));
            }

#if !defined(TARGET_ARM)
            // Unmanaged instance methods on Windows or Unix X86 need the retbuf arg after the first (this) parameter
            if ((TargetOS::IsWindows || compUnixX86Abi()) && srcCall->IsUnmanaged())
            {
                if (callConvIsInstanceMethodCallConv(srcCall->GetUnmanagedCallConv()))
                {
#ifdef TARGET_X86
                    // The argument list has already been reversed. Insert the
                    // return buffer as the second-to-last node  so it will be
                    // pushed on to the stack after the user args but before
                    // the native this arg as required by the native ABI.
                    if (srcCall->gtArgs.Args().begin() == srcCall->gtArgs.Args().end())
                    {
                        // Empty arg list
                        srcCall->gtArgs.PushFront(this, newArg);
                    }
                    else if (srcCall->GetUnmanagedCallConv() == CorInfoCallConvExtension::Thiscall)
                    {
                        // For thiscall, the "this" parameter is not included in the argument list reversal,
                        // so we need to put the return buffer as the last parameter.
                        srcCall->gtArgs.PushBack(this, newArg);
                    }
                    else if (srcCall->gtArgs.Args().begin()->GetNext() == nullptr)
                    {
                        // Only 1 arg, so insert at beginning
                        srcCall->gtArgs.PushFront(this, newArg);
                    }
                    else
                    {
                        // Find second last arg
                        CallArg* secondLastArg = nullptr;
                        for (CallArg& arg : srcCall->gtArgs.Args())
                        {
                            assert(arg.GetNext() != nullptr);
                            if (arg.GetNext()->GetNext() == nullptr)
                            {
                                secondLastArg = &arg;
                                break;
                            }
                        }

                        assert(secondLastArg && "Expected to find second last arg");
                        srcCall->gtArgs.InsertAfter(this, secondLastArg, newArg);
                    }

#else
                    if (srcCall->gtArgs.Args().begin() == srcCall->gtArgs.Args().end())
                    {
                        srcCall->gtArgs.PushFront(this, newArg);
                    }
                    else
                    {
                        srcCall->gtArgs.InsertAfter(this, srcCall->gtArgs.Args().begin().GetArg(), newArg);
                    }
#endif
                }
                else
                {
#ifdef TARGET_X86
                    // The argument list has already been reversed.
                    // Insert the return buffer as the last node so it will be pushed on to the stack last
                    // as required by the native ABI.
                    srcCall->gtArgs.PushBack(this, newArg);
#else
                    // insert the return value buffer into the argument list as first byref parameter
                    srcCall->gtArgs.PushFront(this, newArg);
#endif
                }
            }
            else
#endif // !defined(TARGET_ARM)
            {
                // insert the return value buffer into the argument list as first byref parameter after 'this'
                srcCall->gtArgs.InsertAfterThisOrFirst(this, newArg);
            }

            // now returns void, not a struct
            src->gtType = TYP_VOID;

            // return the morphed call node
            return src;
        }

#ifdef UNIX_AMD64_ABI
        if (store->OperIs(GT_STORE_LCL_VAR))
        {
            // TODO-Cleanup: delete this quirk.
            lvaGetDesc(store->AsLclVar())->lvIsMultiRegRet = true;
        }
#endif // UNIX_AMD64_ABI
    }
    else if (src->OperIs(GT_RET_EXPR))
    {
        assert(src->AsRetExpr()->gtInlineCandidate->OperIs(GT_CALL));
        GenTreeCall* call = src->AsRetExpr()->gtInlineCandidate;

        if (call->ShouldHaveRetBufArg())
        {
            // insert the return value buffer into the argument list as first byref parameter after 'this'
            // TODO-Bug?: verify if flags matter here
            GenTreeFlags indirFlags = GTF_EMPTY;
            GenTree*     destAddr   = impGetNodeAddr(store, CHECK_SPILL_ALL, &indirFlags);

            if (!impIsLegalRetBuf(destAddr, call))
            {
                unsigned tmp = lvaGrabTemp(false DEBUGARG("stack copy for value returned via return buffer"));
                lvaSetStruct(tmp, call->gtRetClsHnd, false);
                destAddr = gtNewLclVarAddrNode(tmp, TYP_I_IMPL);

                // Insert address of temp into existing call
                NewCallArg retBufArg = NewCallArg::Primitive(destAddr).WellKnown(WellKnownArg::RetBuffer);
                call->gtArgs.InsertAfterThisOrFirst(this, retBufArg);

                // Now the store needs to copy from the new temp instead.
                call->gtType      = TYP_VOID;
                src->gtType       = TYP_VOID;
                var_types tmpType = lvaGetDesc(tmp)->TypeGet();
                store->Data()     = gtNewOperNode(GT_COMMA, tmpType, src, gtNewLclvNode(tmp, tmpType));
                return impStoreStruct(store, CHECK_SPILL_ALL, pAfterStmt, di, block);
            }

            call->gtArgs.InsertAfterThisOrFirst(this,
                                                NewCallArg::Primitive(destAddr).WellKnown(WellKnownArg::RetBuffer));

            // now returns void, not a struct
            src->gtType  = TYP_VOID;
            call->gtType = TYP_VOID;

            // We already have appended the write to 'dest' GT_CALL's args
            // So now we just return an empty node (pruning the GT_RET_EXPR)
            return src;
        }
    }
    else if (src->OperIs(GT_COMMA))
    {
        GenTree* sideEffectAddressStore = nullptr;
        if (store->OperIs(GT_STORE_BLK, GT_STOREIND) && ((store->AsIndir()->Addr()->gtFlags & GTF_ALL_EFFECT) != 0))
        {
            TempInfo addrTmp         = fgMakeTemp(store->AsIndir()->Addr());
            sideEffectAddressStore   = addrTmp.store;
            store->AsIndir()->Addr() = addrTmp.load;
        }

        if (pAfterStmt)
        {
            // Insert op1 after '*pAfterStmt'
            if (sideEffectAddressStore != nullptr)
            {
                Statement* addrStmt = gtNewStmt(sideEffectAddressStore, usedDI);
                fgInsertStmtAfter(block, *pAfterStmt, addrStmt);
                *pAfterStmt = addrStmt;
            }

            Statement* newStmt = gtNewStmt(src->AsOp()->gtOp1, usedDI);
            fgInsertStmtAfter(block, *pAfterStmt, newStmt);
            *pAfterStmt = newStmt;
        }
        else if (impLastStmt != nullptr)
        {
            // Do the side-effect as a separate statement.
            if (sideEffectAddressStore != nullptr)
            {
                impAppendTree(sideEffectAddressStore, curLevel, usedDI);
            }
            impAppendTree(src->AsOp()->gtOp1, curLevel, usedDI);
        }
        else
        {
            // In this case we have neither been given a statement to insert after, nor are we
            // in the importer where we can append the side effect.
            // Instead, we're going to sink the store below the COMMA.
            store->Data()      = src->AsOp()->gtOp2;
            src->AsOp()->gtOp2 = impStoreStruct(store, curLevel, pAfterStmt, usedDI, block);
            gtUpdateNodeSideEffects(store);
            src->SetAllEffectsFlags(src->AsOp()->gtOp1, src->AsOp()->gtOp2);

            if (sideEffectAddressStore != nullptr)
            {
                src = gtNewOperNode(GT_COMMA, src->TypeGet(), sideEffectAddressStore, src);
            }
            return src;
        }

        // Evaluate the second thing using recursion.
        store->Data() = src->AsOp()->gtOp2;
        gtUpdateNodeSideEffects(store);
        return impStoreStruct(store, curLevel, pAfterStmt, usedDI, block);
    }

    if (store->OperIs(GT_STORE_LCL_VAR) && src->IsMultiRegNode())
    {
        lvaGetDesc(store->AsLclVar())->SetIsMultiRegDest();
    }

    return store;
}

//------------------------------------------------------------------------
// impIsLegalRetbuf:
//   Check if a return buffer is of a legal shape.
//
// Arguments:
//   retBuf - The return buffer
//   call   - The call that is passed the return buffer
//
// Return Value:
//   True if it is legal according to ABI and IR invariants.
//
// Notes:
//   ABI requires all return buffers to point to stack. Also, we have an IR
//   invariant for async calls that return buffers must be the address of a
//   local.
//
bool Compiler::impIsLegalRetBuf(GenTree* retBuf, GenTreeCall* call)
{
    if (call->IsAsync())
    {
        // Async calls require LCL_ADDR shape for the retbuf to know where to
        // save the value on resumption.
        if (!retBuf->OperIs(GT_LCL_ADDR))
        {
            return false;
        }

        // LCL_ADDR on an implicit byref will turn into LCL_VAR in morph.
        if (lvaIsImplicitByRefLocal(retBuf->AsLclVarCommon()->GetLclNum()))
        {
            return false;
        }

        return true;
    }

    // The ABI requires the retbuffer to point to stack.
    return !fgAddrCouldBeHeap(retBuf) || eeIsByrefLike(call->gtRetClsHnd);
}

//------------------------------------------------------------------------
// impStoreStructPtr: Store (copy) the structure from 'src' to 'destAddr'.
//
// Arguments:
//    destAddr   - address of the destination of the store
//    value      - value to store
//    curLevel   - stack level for which a spill may be being done
//    indirFlags - flags to be used on the store node
//
// Return Value:
//    The tree that should be appended to the statement list that represents the store.
//
// Notes:
//    Temp stores may be appended to impStmtList if spilling is necessary.
//
GenTree* Compiler::impStoreStructPtr(GenTree* destAddr, GenTree* value, unsigned curLevel, GenTreeFlags indirFlags)
{
    var_types    type   = value->TypeGet();
    ClassLayout* layout = (type == TYP_STRUCT) ? value->GetLayout(this) : nullptr;
    GenTree*     store  = gtNewStoreValueNode(type, layout, destAddr, value, indirFlags);
    store               = impStoreStruct(store, curLevel);

    return store;
}

//------------------------------------------------------------------------
// impGetNodeAddr: Get the address of a value.
//
// Arguments:
//    val         - The value in question
//    curLevel    - Stack level for spilling
//    pDerefFlags - Flags to be used on dereference, nullptr when
//                 the address won't be dereferenced. Returned flags
//                 are included in the GTF_IND_COPYABLE_FLAGS mask.
//
// Return Value:
//    In case "val" represents a location (is an indirection/local),
//    will return its address. Otherwise, address of a temporary assigned
//    the value of "val" will be returned.
//
GenTree* Compiler::impGetNodeAddr(GenTree* val, unsigned curLevel, GenTreeFlags* pDerefFlags)
{
    if (pDerefFlags != nullptr)
    {
        *pDerefFlags = GTF_EMPTY;
    }
    switch (val->OperGet())
    {
        case GT_BLK:
        case GT_IND:
        case GT_STOREIND:
        case GT_STORE_BLK:
            if (pDerefFlags != nullptr)
            {
                *pDerefFlags = val->gtFlags & GTF_IND_COPYABLE_FLAGS;
                return val->AsIndir()->Addr();
            }
            break;

        case GT_LCL_VAR:
        case GT_STORE_LCL_VAR:
            val->gtFlags |= GTF_VAR_MOREUSES;
            return gtNewLclVarAddrNode(val->AsLclVar()->GetLclNum(), TYP_BYREF);

        case GT_LCL_FLD:
        case GT_STORE_LCL_FLD:
            val->gtFlags |= GTF_VAR_MOREUSES;
            return gtNewLclAddrNode(val->AsLclFld()->GetLclNum(), val->AsLclFld()->GetLclOffs(), TYP_BYREF);

        case GT_COMMA:
            impAppendTree(val->AsOp()->gtGetOp1(), curLevel, impCurStmtDI);
            return impGetNodeAddr(val->AsOp()->gtGetOp2(), curLevel, pDerefFlags);

        default:
            break;
    }

    unsigned lclNum = lvaGrabTemp(true DEBUGARG("location for address-of(RValue)"));
    impStoreToTemp(lclNum, val, curLevel);

    // The 'return value' is now address of the temp itself.
    return gtNewLclVarAddrNode(lclNum, TYP_BYREF);
}

//------------------------------------------------------------------------
// impNormStructType: Normalize the type of a (known to be) struct class handle.
//
// Arguments:
//    structHnd        - The class handle for the struct type of interest.
//    pSimdBaseJitType - (optional, default nullptr) - if non-null, and the struct is a SIMD
//                       type, set to the SIMD base JIT type
//
// Return Value:
//    The JIT type for the struct (e.g. TYP_STRUCT, or TYP_SIMD*).
//    It may also modify the compFloatingPointUsed flag if the type is a SIMD type.
//
// Notes:
//    Normalizing the type involves examining the struct type to determine if it should
//    be modified to one that is handled specially by the JIT, possibly being a candidate
//    for full enregistration, e.g. TYP_SIMD16. If the size of the struct is already known
//    call structSizeMightRepresentSIMDType to determine if this api needs to be called.
//
var_types Compiler::impNormStructType(CORINFO_CLASS_HANDLE structHnd, CorInfoType* pSimdBaseJitType)
{
    assert(structHnd != NO_CLASS_HANDLE);

    var_types structType = TYP_STRUCT;

#ifdef FEATURE_SIMD
    const DWORD structFlags = info.compCompHnd->getClassAttribs(structHnd);

    // Don't bother if the struct contains GC references of byrefs, it can't be a SIMD type.
    if ((structFlags & (CORINFO_FLG_CONTAINS_GC_PTR | CORINFO_FLG_BYREF_LIKE)) == 0)
    {
        unsigned originalSize = info.compCompHnd->getClassSize(structHnd);

        if (structSizeMightRepresentSIMDType(originalSize))
        {
            unsigned int sizeBytes;
            CorInfoType  simdBaseJitType = getBaseJitTypeAndSizeOfSIMDType(structHnd, &sizeBytes);
            if (simdBaseJitType != CORINFO_TYPE_UNDEF)
            {
                assert(sizeBytes == originalSize);
                structType = getSIMDTypeForSize(sizeBytes);
                if (pSimdBaseJitType != nullptr)
                {
                    *pSimdBaseJitType = simdBaseJitType;
                }
                // Also indicate that we use floating point registers.
                compFloatingPointUsed = true;
            }
        }
    }
#endif // FEATURE_SIMD

    return structType;
}

//------------------------------------------------------------------------
// impNormStructVal: Normalize a struct call argument
//
// Spills call-like STRUCT arguments to temporaries. "Unwraps" commas.
//
// Arguments:
//    structVal - The node to normalize
//    curLevel  - The current stack level
//
// Return Value:
//    The normalized "structVal".
//
GenTree* Compiler::impNormStructVal(GenTree* structVal, unsigned curLevel)
{
    assert(varTypeIsStruct(structVal));
    var_types structType = structVal->TypeGet();

    switch (structVal->OperGet())
    {
        case GT_CALL:
        case GT_RET_EXPR:
        {
            unsigned lclNum = lvaGrabTemp(true DEBUGARG("spilled call-like call argument"));
            impStoreToTemp(lclNum, structVal, curLevel);

            // The structVal is now the temp itself
            structVal = gtNewLclvNode(lclNum, structType);
        }
        break;

        case GT_COMMA:
        {
            GenTree* blockNode = structVal->AsOp()->gtOp2;
            assert(blockNode->gtType == structType);

            // Is this GT_COMMA(op1, GT_COMMA())?
            GenTree* parent = structVal;
            if (blockNode->OperIs(GT_COMMA))
            {
                // Find the last node in the comma chain.
                do
                {
                    assert(blockNode->gtType == structType);
                    parent    = blockNode;
                    blockNode = blockNode->AsOp()->gtOp2;
                } while (blockNode->OperIs(GT_COMMA));
            }

            if (blockNode->OperIsBlk())
            {
                // Sink the GT_COMMA below the blockNode addr.
                // That is GT_COMMA(op1, op2=blockNode) is transformed into
                // blockNode(GT_COMMA(TYP_BYREF, op1, op2's op1)).
                //
                // In case of a chained GT_COMMA case, we sink the last
                // GT_COMMA below the blockNode addr.
                GenTree* blockNodeAddr = blockNode->AsOp()->gtOp1;
                assert(blockNodeAddr->TypeIs(TYP_BYREF, TYP_I_IMPL));
                GenTree* commaNode       = parent;
                commaNode->gtType        = blockNodeAddr->gtType;
                commaNode->AsOp()->gtOp2 = blockNodeAddr;
                blockNode->AsOp()->gtOp1 = commaNode;
                blockNode->AddAllEffectsFlags(commaNode);
                if (parent == structVal)
                {
                    structVal = blockNode;
                }
            }
        }
        break;

        default:
            break;
    }

    return structVal;
}

/******************************************************************************/
// Given a type token, generate code that will evaluate to the correct
// handle representation of that token (type handle, field handle, or method handle)
//
// For most cases, the handle is determined at compile-time, and the code
// generated is simply an embedded handle.
//
// Run-time lookup is required if the enclosing method is shared between instantiations
// and the token refers to formal type parameters whose instantiation is not known
// at compile-time.
//
GenTree* Compiler::impTokenToHandle(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                    bool*                   pRuntimeLookup /* = NULL */,
                                    bool                    mustRestoreHandle /* = false */,
                                    bool                    importParent /* = false */)
{
    assert(!fgGlobalMorph);

    CORINFO_GENERICHANDLE_RESULT embedInfo;
    info.compCompHnd->embedGenericHandle(pResolvedToken, importParent, info.compMethodHnd, &embedInfo);

    if (pRuntimeLookup)
    {
        *pRuntimeLookup = embedInfo.lookup.lookupKind.needsRuntimeLookup;
    }

    if (mustRestoreHandle && !embedInfo.lookup.lookupKind.needsRuntimeLookup)
    {
        switch (embedInfo.handleType)
        {
            case CORINFO_HANDLETYPE_CLASS:
                info.compCompHnd->classMustBeLoadedBeforeCodeIsRun((CORINFO_CLASS_HANDLE)embedInfo.compileTimeHandle);
                break;

            case CORINFO_HANDLETYPE_METHOD:
                info.compCompHnd->methodMustBeLoadedBeforeCodeIsRun((CORINFO_METHOD_HANDLE)embedInfo.compileTimeHandle);
                break;

            case CORINFO_HANDLETYPE_FIELD:
                info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(
                    info.compCompHnd->getFieldClass((CORINFO_FIELD_HANDLE)embedInfo.compileTimeHandle));
                break;

            default:
                break;
        }
    }

    // Generate the full lookup tree. May be null if we're abandoning an inline attempt.
    GenTreeFlags handleType = importParent ? GTF_ICON_CLASS_HDL : gtTokenToIconFlags(pResolvedToken->token);
    GenTree*     result = impLookupToTree(pResolvedToken, &embedInfo.lookup, handleType, embedInfo.compileTimeHandle);

    // If we have a result and it requires runtime lookup, wrap it in a runtime lookup node.
    if ((result != nullptr) && embedInfo.lookup.lookupKind.needsRuntimeLookup)
    {
        result = gtNewRuntimeLookup(embedInfo.compileTimeHandle, embedInfo.handleType, result);
    }

    return result;
}

GenTree* Compiler::impLookupToTree(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                   CORINFO_LOOKUP*         pLookup,
                                   GenTreeFlags            handleFlags,
                                   void*                   compileTimeHandle)
{
    if (!pLookup->lookupKind.needsRuntimeLookup)
    {
        // No runtime lookup is required.
        // Access is direct or memory-indirect (of a fixed address) reference

        CORINFO_GENERIC_HANDLE handle       = nullptr;
        void*                  pIndirection = nullptr;
        assert(pLookup->constLookup.accessType != IAT_PPVALUE && pLookup->constLookup.accessType != IAT_RELPVALUE);

        if (pLookup->constLookup.accessType == IAT_VALUE)
        {
            handle = pLookup->constLookup.handle;
        }
        else if (pLookup->constLookup.accessType == IAT_PVALUE)
        {
            pIndirection = pLookup->constLookup.addr;
        }
        GenTree* addr = gtNewIconEmbHndNode(handle, pIndirection, handleFlags, compileTimeHandle);

#ifdef DEBUG
        size_t handleToTrack;
        if (handleFlags == GTF_ICON_TOKEN_HDL)
        {
            handleToTrack = 0;
        }
        else
        {
            handleToTrack = (size_t)compileTimeHandle;
        }

        if (handle != nullptr)
        {
            addr->AsIntCon()->gtTargetHandle = handleToTrack;
        }
        else
        {
            addr->gtGetOp1()->AsIntCon()->gtTargetHandle = handleToTrack;
        }
#endif
        return addr;
    }

    if (pLookup->lookupKind.runtimeLookupKind == CORINFO_LOOKUP_NOT_SUPPORTED)
    {
        // Runtime does not support inlining of all shapes of runtime lookups
        // Inlining has to be aborted in such a case
        assert(compIsForInlining());
        compInlineResult->NoteFatal(InlineObservation::CALLSITE_GENERIC_DICTIONARY_LOOKUP);
        return nullptr;
    }

    // Need to use dictionary-based access which depends on the typeContext
    // which is only available at runtime, not at compile-time.
    return impRuntimeLookupToTree(pResolvedToken, pLookup, compileTimeHandle);
}

#ifdef FEATURE_READYTORUN
GenTree* Compiler::impReadyToRunLookupToTree(CORINFO_CONST_LOOKUP* pLookup,
                                             GenTreeFlags          handleFlags,
                                             void*                 compileTimeHandle)
{
    CORINFO_GENERIC_HANDLE handle       = nullptr;
    void*                  pIndirection = nullptr;
    assert(pLookup->accessType != IAT_PPVALUE && pLookup->accessType != IAT_RELPVALUE);

    if (pLookup->accessType == IAT_VALUE)
    {
        handle = pLookup->handle;
    }
    else if (pLookup->accessType == IAT_PVALUE)
    {
        pIndirection = pLookup->addr;
    }
    GenTree* addr = gtNewIconEmbHndNode(handle, pIndirection, handleFlags, compileTimeHandle);
#ifdef DEBUG
    assert((handleFlags == GTF_ICON_CLASS_HDL) || (handleFlags == GTF_ICON_METHOD_HDL));
    if (handle != nullptr)
    {
        addr->AsIntCon()->gtTargetHandle = (size_t)compileTimeHandle;
    }
    else
    {
        addr->gtGetOp1()->AsIntCon()->gtTargetHandle = (size_t)compileTimeHandle;
    }
#endif //  DEBUG
    return addr;
}

//------------------------------------------------------------------------
// impIsCastHelperEligibleForClassProbe: Checks whether a tree is a cast helper eligible to
//    to be profiled and then optimized with PGO data
//
// Arguments:
//    tree - the tree object to check
//
// Returns:
//    true if the tree is a cast helper eligible to be profiled
//
bool Compiler::impIsCastHelperEligibleForClassProbe(GenTree* tree)
{
    if (!opts.IsInstrumented() || (JitConfig.JitProfileCasts() != 1))
    {
        return false;
    }

    if (tree->IsHelperCall())
    {
        switch (eeGetHelperNum(tree->AsCall()->gtCallMethHnd))
        {
            case CORINFO_HELP_ISINSTANCEOFINTERFACE:
            case CORINFO_HELP_ISINSTANCEOFARRAY:
            case CORINFO_HELP_ISINSTANCEOFCLASS:
            case CORINFO_HELP_ISINSTANCEOFANY:
            case CORINFO_HELP_CHKCASTINTERFACE:
            case CORINFO_HELP_CHKCASTARRAY:
            case CORINFO_HELP_CHKCASTCLASS:
            case CORINFO_HELP_CHKCASTANY:
                return true;
            default:
                break;
        }
    }
    return false;
}

//------------------------------------------------------------------------
// impIsCastHelperMayHaveProfileData: Checks whether a tree is a cast helper that might
//    have profile data
//
// Arguments:
//    tree - the tree object to check
//
// Returns:
//    true if the tree is a cast helper with potential profile data
//
bool Compiler::impIsCastHelperMayHaveProfileData(CorInfoHelpFunc helper)
{
    if ((JitConfig.JitConsumeProfileForCasts() != 1) || !opts.jitFlags->IsSet(JitFlags::JIT_FLAG_BBOPT))
    {
        return false;
    }

    switch (helper)
    {
        case CORINFO_HELP_ISINSTANCEOFINTERFACE:
        case CORINFO_HELP_ISINSTANCEOFARRAY:
        case CORINFO_HELP_ISINSTANCEOFCLASS:
        case CORINFO_HELP_ISINSTANCEOFANY:
        case CORINFO_HELP_CHKCASTINTERFACE:
        case CORINFO_HELP_CHKCASTARRAY:
        case CORINFO_HELP_CHKCASTCLASS:
        case CORINFO_HELP_CHKCASTANY:
            return true;
        default:
            return false;
    }
}

GenTreeCall* Compiler::impReadyToRunHelperToTree(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                                 CorInfoHelpFunc         helper,
                                                 var_types               type,
                                                 CORINFO_LOOKUP_KIND*    pGenericLookupKind,
                                                 GenTree*                arg1)
{
    CORINFO_CONST_LOOKUP lookup;
    if (!info.compCompHnd->getReadyToRunHelper(pResolvedToken, pGenericLookupKind, helper, info.compMethodHnd, &lookup))
    {
        return nullptr;
    }

    GenTreeCall* op1 = gtNewHelperCallNode(helper, type, arg1);

    op1->setEntryPoint(lookup);

    if (IsStaticHelperEligibleForExpansion(op1))
    {
        // Keep class handle attached to the helper call since it's difficult to restore it
        // Keep class handle attached to the helper call since it's difficult to restore it.
        op1->gtInitClsHnd = pResolvedToken->hClass;
    }

    return op1;
}
#endif

GenTree* Compiler::impMethodPointer(CORINFO_RESOLVED_TOKEN* pResolvedToken, CORINFO_CALL_INFO* pCallInfo)
{
    GenTree* op1 = nullptr;

    switch (pCallInfo->kind)
    {
        case CORINFO_CALL:
            op1 = new (this, GT_FTN_ADDR) GenTreeFptrVal(TYP_I_IMPL, pCallInfo->hMethod);

#ifdef FEATURE_READYTORUN
            if (IsAot())
            {
                op1->AsFptrVal()->gtEntryPoint = pCallInfo->codePointerLookup.constLookup;
            }
#endif
            break;

        case CORINFO_CALL_CODE_POINTER:
            op1 = impLookupToTree(pResolvedToken, &pCallInfo->codePointerLookup, GTF_ICON_FTN_ADDR, pCallInfo->hMethod);
            break;

        default:
            noway_assert(!"unknown call kind");
            break;
    }

    return op1;
}

//------------------------------------------------------------------------
// getRuntimeContextTree: find pointer to context for runtime lookup.
//
// Arguments:
//    kind - lookup kind.
//
// Return Value:
//    Return GenTree pointer to generic shared context.
//
// Notes:
//    Reports about generic context using.

GenTree* Compiler::getRuntimeContextTree(CORINFO_RUNTIME_LOOKUP_KIND kind)
{
    GenTree* ctxTree;

    // Collectible types requires that for shared generic code, if we use the generic context parameter
    // that we report it. Conservatively mark the root method as using generic context, MARK_LOCAL_VARS phase
    // will clean it up if it turns out to be unnecessary.
    impInlineRoot()->lvaGenericsContextInUse = true;

    // Always use generic context from the callsite if we're inlining and it's available.
    if (compIsForInlining() && (impInlineInfo->inlInstParamArgInfo != nullptr))
    {
        // Create a dummy lclInfo node, we know that nobody's going to do stloc or take address
        // of the generic context, so we don't need to scan IL for it.
        InlLclVarInfo lclInfo = {};
        lclInfo.lclTypeInfo   = TYP_I_IMPL;
        ctxTree               = impInlineFetchArg(*impInlineInfo->inlInstParamArgInfo, lclInfo);
        assert(ctxTree != nullptr);
        assert(ctxTree->TypeIs(TYP_I_IMPL));
        // We don't need to worry about GTF_VAR_CONTEXT here, it should be set on the callsite anyway.
    }
    else if (kind == CORINFO_LOOKUP_THISOBJ)
    {
        // Use "this" from the callsite if we're inlining
        if (compIsForInlining())
        {
            // "this" is always the first argument in inlArgInfo
            assert(impInlineInfo->argCnt > 0);
            assert(impInlineInfo->inlArgInfo[0].argIsThis);

            ctxTree = impInlineFetchArg(impInlineInfo->inlArgInfo[0], impInlineInfo->lclVarInfo[0]);

            // "this" is expected to be always a local, and we must mark it as a context
            assert(ctxTree->OperIs(GT_LCL_VAR));
            ctxTree->gtFlags |= GTF_VAR_CONTEXT;
        }
        else
        {
            assert(info.compThisArg != BAD_VAR_NUM);
            ctxTree = gtNewLclvNode(info.compThisArg, TYP_REF);
            ctxTree->gtFlags |= GTF_VAR_CONTEXT;
        }

        // context is the method table pointer of the this object
        ctxTree = gtNewMethodTableLookup(ctxTree);
    }
    else
    {
        assert((kind == CORINFO_LOOKUP_METHODPARAM) || (kind == CORINFO_LOOKUP_CLASSPARAM));

        // Exact method descriptor as passed in
        ctxTree = gtNewLclvNode(impInlineRoot()->info.compTypeCtxtArg, TYP_I_IMPL);
        ctxTree->gtFlags |= GTF_VAR_CONTEXT;
    }
    return ctxTree;
}

/*****************************************************************************/
/* Import a dictionary lookup to access a handle in code shared between
   generic instantiations.
   The lookup depends on the typeContext which is only available at
   runtime, and not at compile-time.
   pLookup->token1 and pLookup->token2 specify the handle that is needed.
   The cases are:

   1. pLookup->indirections == CORINFO_USEHELPER : Call a helper passing it the
      instantiation-specific handle, and the tokens to lookup the handle.
   2. pLookup->indirections == CORINFO_USENULL : Pass null. Callee won't dereference
      the context.
   3. pLookup->indirections != CORINFO_USEHELPER or CORINFO_USENULL :
      2a. pLookup->testForNull == false : Dereference the instantiation-specific handle
          to get the handle.
      2b. pLookup->testForNull == true : Dereference the instantiation-specific handle.
          If it is non-NULL, it is the handle required. Else, call a helper
          to lookup the handle.
 */

GenTree* Compiler::impRuntimeLookupToTree(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                          CORINFO_LOOKUP*         pLookup,
                                          void*                   compileTimeHandle)
{
    GenTree* ctxTree = getRuntimeContextTree(pLookup->lookupKind.runtimeLookupKind);

    CORINFO_RUNTIME_LOOKUP* pRuntimeLookup = &pLookup->runtimeLookup;
    // It's available only via the run-time helper function
    if (pRuntimeLookup->indirections == CORINFO_USEHELPER)
    {
#ifdef FEATURE_READYTORUN
        if (IsAot())
        {
            return impReadyToRunHelperToTree(pResolvedToken, CORINFO_HELP_READYTORUN_GENERIC_HANDLE, TYP_I_IMPL,
                                             &pLookup->lookupKind, ctxTree);
        }
#endif
        return gtNewRuntimeLookupHelperCallNode(pRuntimeLookup, ctxTree, compileTimeHandle);
    }

#ifdef FEATURE_READYTORUN
    if (pRuntimeLookup->indirections == CORINFO_USENULL)
    {
        return gtNewIconNode(0, TYP_I_IMPL);
    }
#endif

    if (pRuntimeLookup->testForNull)
    {
        // Import just a helper call and mark it for late expansion in fgExpandRuntimeLookups phase
        assert(pRuntimeLookup->indirections != 0);
        GenTreeCall* helperCall = gtNewRuntimeLookupHelperCallNode(pRuntimeLookup, ctxTree, compileTimeHandle);

        // Spilling it to a temp improves CQ (mainly in Tier0)
        unsigned callLclNum = lvaGrabTemp(true DEBUGARG("spilling helperCall"));
        impStoreToTemp(callLclNum, helperCall, CHECK_SPILL_NONE);
        return gtNewLclvNode(callLclNum, helperCall->TypeGet());
    }

    // Size-check is not expected without testForNull
    assert(pRuntimeLookup->sizeOffset == CORINFO_NO_SIZE_CHECK);

    // Slot pointer
    GenTree* slotPtrTree = ctxTree;
    GenTree* indOffTree  = nullptr;

    // TODO-CQ: consider relaxing where it's safe to do so
    const bool ctxTreeIsInvariant = !compIsForInlining();

    // Applied repeated indirections
    for (WORD i = 0; i < pRuntimeLookup->indirections; i++)
    {
        if ((i == 1 && pRuntimeLookup->indirectFirstOffset) || (i == 2 && pRuntimeLookup->indirectSecondOffset))
        {
            indOffTree = impCloneExpr(slotPtrTree, &slotPtrTree, CHECK_SPILL_ALL,
                                      nullptr DEBUGARG("impRuntimeLookup indirectOffset"));
        }

        if (i != 0)
        {
            slotPtrTree = gtNewIndir(TYP_I_IMPL, slotPtrTree,
                                     ctxTreeIsInvariant ? (GTF_IND_NONFAULTING | GTF_IND_INVARIANT) : GTF_EMPTY);
        }

        if ((i == 1 && pRuntimeLookup->indirectFirstOffset) || (i == 2 && pRuntimeLookup->indirectSecondOffset))
        {
            slotPtrTree = gtNewOperNode(GT_ADD, TYP_I_IMPL, indOffTree, slotPtrTree);
        }

        if (pRuntimeLookup->offsets[i] != 0)
        {
            slotPtrTree =
                gtNewOperNode(GT_ADD, TYP_I_IMPL, slotPtrTree, gtNewIconNode(pRuntimeLookup->offsets[i], TYP_I_IMPL));
        }
    }

    // No null test required
    assert(!pRuntimeLookup->testForNull);

    if (pRuntimeLookup->indirections == 0)
    {
        return slotPtrTree;
    }

    slotPtrTree =
        gtNewIndir(TYP_I_IMPL, slotPtrTree, ctxTreeIsInvariant ? (GTF_IND_NONFAULTING | GTF_IND_INVARIANT) : GTF_EMPTY);

    return slotPtrTree;
}

struct RecursiveGuard
{
public:
    RecursiveGuard()
    {
        m_pAddress = nullptr;
    }

    ~RecursiveGuard()
    {
        if (m_pAddress)
        {
            *m_pAddress = false;
        }
    }

    void Init(bool* pAddress, bool bInitialize)
    {
        assert(pAddress && *pAddress == false && "Recursive guard violation");
        m_pAddress = pAddress;

        if (bInitialize)
        {
            *m_pAddress = true;
        }
    }

protected:
    bool* m_pAddress;
};

bool Compiler::impSpillStackEntry(unsigned level,
                                  unsigned tnum
#ifdef DEBUG
                                  ,
                                  bool        bAssertOnRecursion,
                                  const char* reason
#endif
)
{

#ifdef DEBUG
    RecursiveGuard guard;
    guard.Init(&impNestedStackSpill, bAssertOnRecursion);
#endif

    GenTree* tree = stackState.esStack[level].val;

    /* Allocate a temp if we haven't been asked to use a particular one */

    if (tnum != BAD_VAR_NUM && (tnum >= lvaCount))
    {
        return false;
    }

    bool isNewTemp = false;

    if (tnum == BAD_VAR_NUM)
    {
        tnum      = lvaGrabTemp(true DEBUGARG(reason));
        isNewTemp = true;
    }

    /* Assign the spilled entry to the temp */
    impStoreToTemp(tnum, tree, level);

    if (isNewTemp)
    {
        assert(lvaTable[tnum].lvSingleDef == 0);
        lvaTable[tnum].lvSingleDef = 1;
        JITDUMP("Marked V%02u as a single def temp\n", tnum);

        // If temp is newly introduced and a ref type, grab what type info we can.
        if (lvaTable[tnum].lvType == TYP_REF)
        {
            CORINFO_CLASS_HANDLE stkHnd = stackState.esStack[level].seTypeInfo.GetClassHandleForObjRef();
            lvaSetClass(tnum, tree, stkHnd);
        }

        // If we're assigning a GT_RET_EXPR, note the temp over on the call,
        // so the inliner can use it in case it needs a return spill temp.
        if (tree->OperIs(GT_RET_EXPR))
        {
            JITDUMP("\n*** see V%02u = GT_RET_EXPR, noting temp\n", tnum);
            GenTreeCall* call = tree->AsRetExpr()->gtInlineCandidate->AsCall();
            if (call->IsGuardedDevirtualizationCandidate())
            {
                for (uint8_t i = 0; i < call->GetInlineCandidatesCount(); i++)
                {
                    call->GetGDVCandidateInfo(i)->preexistingSpillTemp = tnum;
                }
            }
            else
            {
                call->AsCall()->GetSingleInlineCandidateInfo()->preexistingSpillTemp = tnum;
            }
        }
    }

    // The tree type may be modified by impStoreToTemp, so use the type of the lclVar.
    var_types type                = genActualType(lvaTable[tnum].TypeGet());
    GenTree*  temp                = gtNewLclvNode(tnum, type);
    stackState.esStack[level].val = temp;

    return true;
}

/*****************************************************************************
 *
 *  Ensure that the stack has only spilled values
 */

void Compiler::impSpillStackEnsure(bool spillLeaves)
{
    assert(!spillLeaves || opts.compDbgCode);

    for (unsigned level = 0; level < stackState.esStackDepth; level++)
    {
        GenTree* tree = stackState.esStack[level].val;

        if (!spillLeaves && tree->OperIsLeaf())
        {
            continue;
        }

        // Temps introduced by the importer itself don't need to be spilled

        bool isTempLcl = tree->OperIs(GT_LCL_VAR) && (tree->AsLclVarCommon()->GetLclNum() >= info.compLocalsCount);

        if (isTempLcl)
        {
            continue;
        }

        impSpillStackEntry(level, BAD_VAR_NUM DEBUGARG(false) DEBUGARG("impSpillStackEnsure"));
    }
}

/*****************************************************************************
 *
 *  If the stack contains any trees with side effects in them, assign those
 *  trees to temps and append the stores to the statement list.
 *  On return the stack is guaranteed to be empty.
 */

void Compiler::impEvalSideEffects()
{
    impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("impEvalSideEffects"));
    stackState.esStackDepth = 0;
}

/*****************************************************************************
 *
 *  If the stack entry is a tree with side effects in it, assign that
 *  tree to a temp and replace it on the stack with refs to its temp.
 *  i is the stack entry which will be checked and spilled.
 */

void Compiler::impSpillSideEffect(bool spillGlobEffects, unsigned i DEBUGARG(const char* reason))
{
    assert(i <= stackState.esStackDepth);

    GenTreeFlags spillFlags = spillGlobEffects ? GTF_GLOB_EFFECT : GTF_SIDE_EFFECT;
    GenTree*     tree       = stackState.esStack[i].val;

    if ((tree->gtFlags & spillFlags) != 0 ||
        (spillGlobEffects &&           // Only consider the following when  spillGlobEffects == true
         !impIsAddressInLocal(tree) && // No need to spill the LCL_ADDR nodes.
         gtHasLocalsWithAddrOp(tree))) // Spill if we still see GT_LCL_VAR that contains lvHasLdAddrOp or
                                       // lvAddrTaken flag.
    {
        impSpillStackEntry(i, BAD_VAR_NUM DEBUGARG(false) DEBUGARG(reason));
    }
}

/*****************************************************************************
 *
 *  If the stack contains any trees with side effects in them, assign those
 *  trees to temps and replace them on the stack with refs to their temps.
 *  [0..chkLevel) is the portion of the stack which will be checked and spilled.
 */

void Compiler::impSpillSideEffects(bool spillGlobEffects, unsigned chkLevel DEBUGARG(const char* reason))
{
    assert(chkLevel != CHECK_SPILL_NONE);

    /* Before we make any appends to the tree list we must spill the
     * "special" side effects (GTF_ORDER_SIDEEFF on a GT_CATCH_ARG) */

    impSpillSpecialSideEff();

    if (chkLevel == CHECK_SPILL_ALL)
    {
        chkLevel = stackState.esStackDepth;
    }

    assert(chkLevel <= stackState.esStackDepth);

    for (unsigned i = 0; i < chkLevel; i++)
    {
        impSpillSideEffect(spillGlobEffects, i DEBUGARG(reason));
    }
}

/*****************************************************************************
 *
 *  If the stack contains any trees with special side effects in them, assign
 *  those trees to temps and replace them on the stack with refs to their temps.
 */

void Compiler::impSpillSpecialSideEff()
{
    // Only exception objects need to be carefully handled

    if (!compCurBB->bbCatchTyp)
    {
        return;
    }

    for (unsigned level = 0; level < stackState.esStackDepth; level++)
    {
        GenTree* tree = stackState.esStack[level].val;
        // Make sure if we have an exception object in the sub tree we spill ourselves.
        if (gtHasCatchArg(tree))
        {
            impSpillStackEntry(level, BAD_VAR_NUM DEBUGARG(false) DEBUGARG("impSpillSpecialSideEff"));
        }
    }
}

//------------------------------------------------------------------------
// impSpillLclRefs: Spill all trees referencing the given local.
//
// Arguments:
//    lclNum   - The local's number
//    chkLevel - Height (exclusive) of the portion of the stack to check
//
void Compiler::impSpillLclRefs(unsigned lclNum, unsigned chkLevel)
{
    // Before we make any appends to the tree list we must spill the
    // "special" side effects (GTF_ORDER_SIDEEFF) - GT_CATCH_ARG.
    impSpillSpecialSideEff();

    if (chkLevel == CHECK_SPILL_ALL)
    {
        chkLevel = stackState.esStackDepth;
    }

    assert(chkLevel <= stackState.esStackDepth);

    for (unsigned level = 0; level < chkLevel; level++)
    {
        GenTree* tree = stackState.esStack[level].val;

        /* If the tree may throw an exception, and the block has a handler,
           then we need to spill stores to the local if the local is on entry
           to the handler. Just spill 'em all without considering the liveness */

        bool xcptnCaught = ehBlockHasExnFlowDsc(compCurBB) && (tree->gtFlags & (GTF_CALL | GTF_EXCEPT));

        /* Skip the tree if it doesn't have an affected reference,
           unless xcptnCaught */

        if (xcptnCaught || gtHasRef(tree, lclNum))
        {
            impSpillStackEntry(level, BAD_VAR_NUM DEBUGARG(false) DEBUGARG("impSpillLclRefs"));
        }
    }
}

//------------------------------------------------------------------------
// impPushCatchArgOnStack: Push catch arg onto the stack.
//
// Arguments:
//   hndBlk - first block of the catch handler
//   clsHnd - type being caught
//   isSingleBlockFilter - true if catch has single block filtger
//
// Returns:
//   the basic block of the actual handler.
//
// Notes:
//  If there are jumps to the beginning of the handler, insert basic block
//  and spill catch arg to a temp. Update the handler block if necessary.
//
BasicBlock* Compiler::impPushCatchArgOnStack(BasicBlock* hndBlk, CORINFO_CLASS_HANDLE clsHnd, bool isSingleBlockFilter)
{
    // Do not inject the basic block twice on reimport. This should be
    // hit only under JIT stress. See if the block is the one we injected.
    // Note that EH canonicalization can inject internal blocks here. We might
    // be able to re-use such a block (but we don't, right now).
    if (hndBlk->HasAllFlags(BBF_IMPORTED | BBF_INTERNAL | BBF_DONT_REMOVE))
    {
        Statement* stmt = hndBlk->firstStmt();

        if (stmt != nullptr)
        {
            GenTree* tree = stmt->GetRootNode();
            assert(tree != nullptr);

            if (tree->OperIs(GT_STORE_LCL_VAR) && tree->AsLclVar()->Data()->OperIs(GT_CATCH_ARG))
            {
                tree = gtNewLclvNode(tree->AsLclVar()->GetLclNum(), TYP_REF);

                impPushOnStack(tree, typeInfo(clsHnd));

                return hndBlk->Next();
            }
        }

        // If we get here, it must have been some other kind of internal block. It's possible that
        // someone prepended something to our injected block, but that's unlikely.
    }

    /* Push the exception address value on the stack */
    GenTree* arg = new (this, GT_CATCH_ARG) GenTree(GT_CATCH_ARG, TYP_REF);

    /* Mark the node as having a side-effect - i.e. cannot be
     * moved around since it is tied to a fixed location (EAX) */
    arg->SetHasOrderingSideEffect();

#if defined(JIT32_GCENCODER)
    const bool forceInsertNewBlock = isSingleBlockFilter || compStressCompile(STRESS_CATCH_ARG, 5);
#else
    const bool forceInsertNewBlock = compStressCompile(STRESS_CATCH_ARG, 5);
#endif // defined(JIT32_GCENCODER)

    // Spill GT_CATCH_ARG to a temp if there are jumps to the beginning of the handler.
    //
    // For typical normal handlers we expect ref count to be 2 here (one artificial, one for
    // the edge from the xxx...)
    //
    if ((hndBlk->bbRefs > 2) || forceInsertNewBlock)
    {
        // Create extra basic block for the spill
        //
        BasicBlock* newBlk = fgNewBBbefore(BBJ_ALWAYS, hndBlk, /* extendRegion */ true);
        newBlk->SetFlags(BBF_IMPORTED | BBF_DONT_REMOVE);
        newBlk->inheritWeight(hndBlk);
        newBlk->bbCodeOffs = hndBlk->bbCodeOffs;

        FlowEdge* const newEdge = fgAddRefPred(hndBlk, newBlk);
        newBlk->SetTargetEdge(newEdge);

        // Spill into a temp.
        unsigned tempNum         = lvaGrabTemp(false DEBUGARG("SpillCatchArg"));
        lvaTable[tempNum].lvType = TYP_REF;
        GenTree* argStore        = gtNewTempStore(tempNum, arg);
        arg                      = gtNewLclvNode(tempNum, TYP_REF);

        hndBlk->bbStkTempsIn = tempNum;

        Statement* argStmt;

        if (info.compStmtOffsetsImplicit & ICorDebugInfo::CALL_SITE_BOUNDARIES)
        {
            // Report the debug info. impImportBlockCode won't treat the actual handler as exception block and thus
            // won't do it for us.
            // TODO-DEBUGINFO: Previous code always set stack as non-empty
            // here. Can we not just use impCurStmtOffsSet? Are we out of sync
            // here with the stack?
            impCurStmtDI = DebugInfo(compInlineContext, ILLocation(newBlk->bbCodeOffs, false, false));
            argStmt      = gtNewStmt(argStore, impCurStmtDI);
        }
        else
        {
            argStmt = gtNewStmt(argStore);
        }

        fgInsertStmtAtEnd(newBlk, argStmt);
    }

    impPushOnStack(arg, typeInfo(clsHnd));

    return hndBlk;
}

/*****************************************************************************
 *
 *  Given a tree, clone it. *pClone is set to the cloned tree.
 *  Returns the original tree if the cloning was easy,
 *   else returns the temp to which the tree had to be spilled to.
 *  If the tree has side-effects, it will be spilled to a temp.
 */

GenTree* Compiler::impCloneExpr(GenTree*               tree,
                                GenTree**              pClone,
                                unsigned               curLevel,
                                Statement** pAfterStmt DEBUGARG(const char* reason))
{
    if (!(tree->gtFlags & GTF_GLOB_EFFECT))
    {
        GenTree* clone = gtClone(tree, true);

        if (clone)
        {
            *pClone = clone;
            return tree;
        }
    }

    /* Store the operand in a temp and return the temp */

    unsigned temp = lvaGrabTemp(true DEBUGARG(reason));

    // impStoreToTemp() may change tree->gtType to TYP_VOID for calls which
    // return a struct type. It also may modify the struct type to a more
    // specialized type (e.g. a SIMD type).  So we will get the type from
    // the lclVar AFTER calling impStoreToTemp().

    impStoreToTemp(temp, tree, curLevel, pAfterStmt, impCurStmtDI);
    var_types type = genActualType(lvaTable[temp].TypeGet());

    *pClone = gtNewLclvNode(temp, type);
    return gtNewLclvNode(temp, type);
}

//------------------------------------------------------------------------
// impCreateDIWithCurrentStackInfo: Create a DebugInfo instance with the
// specified IL offset and 'is call' bit, using the current stack to determine
// whether to set the 'stack empty' bit.
//
// Arguments:
//    offs   - the IL offset for the DebugInfo
//    isCall - whether the created DebugInfo should have the IsCall bit set
//
// Return Value:
//    The DebugInfo instance.
//
DebugInfo Compiler::impCreateDIWithCurrentStackInfo(IL_OFFSET offs, bool isCall)
{
    assert(offs != BAD_IL_OFFSET);

    bool isStackEmpty = stackState.esStackDepth <= 0;
    return DebugInfo(compInlineContext, ILLocation(offs, isStackEmpty, isCall));
}

//------------------------------------------------------------------------
// impCurStmtOffsSet: Set the "current debug info" to attach to statements that
// we are generating next.
//
// Arguments:
//    offs - the IL offset
//
// Remarks:
//    This function will be called in the main IL processing loop when it is
//    determined that we have reached a location in the IL stream for which we
//    want to report debug information. This is the main way we determine which
//    statements to report debug info for to the EE: for other statements, they
//    will have no debug information attached.
//
void Compiler::impCurStmtOffsSet(IL_OFFSET offs)
{
    if (offs == BAD_IL_OFFSET)
    {
        impCurStmtDI = DebugInfo(compInlineContext, ILLocation());
    }
    else
    {
        impCurStmtDI = impCreateDIWithCurrentStackInfo(offs, false);
    }
}

//------------------------------------------------------------------------
// impCanSpillNow: check is it possible to spill all values from eeStack to local variables.
//
// Arguments:
//    prevOpcode - last importer opcode
//
// Return Value:
//    true if it is legal, false if it could be a sequence that we do not want to divide.
bool Compiler::impCanSpillNow(OPCODE prevOpcode)
{
    // Don't spill after ldtoken, newarr and newobj, because it could be a part of the InitializeArray sequence.
    // Avoid breaking up to guarantee that impInitializeArrayIntrinsic can succeed.
    return (prevOpcode != CEE_LDTOKEN) && (prevOpcode != CEE_NEWARR) && (prevOpcode != CEE_NEWOBJ);
}

/*****************************************************************************
 *
 *  Remember the instr offset for the statements
 *
 *  When we do impAppendTree(tree), we can't set stmt->SetLastILOffset(impCurOpcOffs),
 *  if the append was done because of a partial stack spill,
 *  as some of the trees corresponding to code up to impCurOpcOffs might
 *  still be sitting on the stack.
 *  So we delay calling of SetLastILOffset() until impNoteLastILoffs().
 *  This should be called when an opcode finally/explicitly causes
 *  impAppendTree(tree) to be called (as opposed to being called because of
 *  a spill caused by the opcode)
 */

#ifdef DEBUG

void Compiler::impNoteLastILoffs()
{
    if (impLastILoffsStmt == nullptr)
    {
        // We should have added a statement for the current basic block
        // Is this assert correct ?

        assert(impLastStmt);

        impLastStmt->SetLastILOffset(compIsForInlining() ? BAD_IL_OFFSET : impCurOpcOffs);
    }
    else
    {
        impLastILoffsStmt->SetLastILOffset(compIsForInlining() ? BAD_IL_OFFSET : impCurOpcOffs);
        impLastILoffsStmt = nullptr;
    }
}

#endif // DEBUG

/*****************************************************************************
 * We don't create any GenTree (excluding spills) for a branch.
 * For debugging info, we need a placeholder so that we can note
 * the IL offset in gtStmt.gtStmtOffs. So append an empty statement.
 */

void Compiler::impNoteBranchOffs()
{
    if (opts.compDbgCode)
    {
        impAppendTree(gtNewNothingNode(), CHECK_SPILL_NONE, impCurStmtDI);
    }
}

/*****************************************************************************
 * Locate the next stmt boundary for which we need to record info.
 * We will have to spill the stack at such boundaries if it is not
 * already empty.
 * Returns the next stmt boundary (after the start of the block)
 */

unsigned Compiler::impInitBlockLineInfo()
{
    /* Assume the block does not correspond with any IL offset. This prevents
       us from reporting extra offsets. Extra mappings can cause confusing
       stepping, especially if the extra mapping is a jump-target, and the
       debugger does not ignore extra mappings, but instead rewinds to the
       nearest known offset */

    impCurStmtOffsSet(BAD_IL_OFFSET);

    IL_OFFSET blockOffs = compCurBB->bbCodeOffs;

    if ((stackState.esStackDepth == 0) && (info.compStmtOffsetsImplicit & ICorDebugInfo::STACK_EMPTY_BOUNDARIES))
    {
        impCurStmtOffsSet(blockOffs);
    }

    /* Always report IL offset 0 or some tests get confused.
       Probably a good idea anyways */

    if (blockOffs == 0)
    {
        impCurStmtOffsSet(blockOffs);
    }

    if (!info.compStmtOffsetsCount)
    {
        return ~0;
    }

    /* Find the lowest explicit stmt boundary within the block */

    /* Start looking at an entry that is based on our instr offset */

    unsigned index = (info.compStmtOffsetsCount * blockOffs) / info.compILCodeSize;

    if (index >= info.compStmtOffsetsCount)
    {
        index = info.compStmtOffsetsCount - 1;
    }

    /* If we've guessed too far, back up */

    while (index > 0 && info.compStmtOffsets[index - 1] >= blockOffs)
    {
        index--;
    }

    /* If we guessed short, advance ahead */

    while (info.compStmtOffsets[index] < blockOffs)
    {
        index++;

        if (index == info.compStmtOffsetsCount)
        {
            return info.compStmtOffsetsCount;
        }
    }

    assert(index < info.compStmtOffsetsCount);

    if (info.compStmtOffsets[index] == blockOffs)
    {
        /* There is an explicit boundary for the start of this basic block.
           So we will start with bbCodeOffs. Else we will wait until we
           get to the next explicit boundary */

        impCurStmtOffsSet(blockOffs);

        index++;
    }

    return index;
}

/*****************************************************************************/

bool Compiler::impOpcodeIsCallOpcode(OPCODE opcode)
{
    switch (opcode)
    {
        case CEE_CALL:
        case CEE_CALLI:
        case CEE_CALLVIRT:
            return true;

        default:
            return false;
    }
}

/*****************************************************************************/

static bool impOpcodeIsCallSiteBoundary(OPCODE opcode)
{
    switch (opcode)
    {
        case CEE_CALL:
        case CEE_CALLI:
        case CEE_CALLVIRT:
        case CEE_JMP:
        case CEE_NEWOBJ:
        case CEE_NEWARR:
            return true;

        default:
            return false;
    }
}

/*****************************************************************************/

// One might think it is worth caching these values, but results indicate
// that it isn't.
// In addition, caching them causes SuperPMI to be unable to completely
// encapsulate an individual method context.
CORINFO_CLASS_HANDLE Compiler::impGetRefAnyClass()
{
    CORINFO_CLASS_HANDLE refAnyClass = info.compCompHnd->getBuiltinClass(CLASSID_TYPED_BYREF);
    assert(refAnyClass != (CORINFO_CLASS_HANDLE) nullptr);
    return refAnyClass;
}

CORINFO_CLASS_HANDLE Compiler::impGetTypeHandleClass()
{
    CORINFO_CLASS_HANDLE typeHandleClass = info.compCompHnd->getBuiltinClass(CLASSID_TYPE_HANDLE);
    assert(typeHandleClass != (CORINFO_CLASS_HANDLE) nullptr);
    return typeHandleClass;
}

CORINFO_CLASS_HANDLE Compiler::impGetRuntimeArgumentHandle()
{
    CORINFO_CLASS_HANDLE argIteratorClass = info.compCompHnd->getBuiltinClass(CLASSID_ARGUMENT_HANDLE);
    assert(argIteratorClass != (CORINFO_CLASS_HANDLE) nullptr);
    return argIteratorClass;
}

CORINFO_CLASS_HANDLE Compiler::impGetStringClass()
{
    CORINFO_CLASS_HANDLE stringClass = info.compCompHnd->getBuiltinClass(CLASSID_STRING);
    assert(stringClass != (CORINFO_CLASS_HANDLE) nullptr);
    return stringClass;
}

CORINFO_CLASS_HANDLE Compiler::impGetObjectClass()
{
    CORINFO_CLASS_HANDLE objectClass = info.compCompHnd->getBuiltinClass(CLASSID_SYSTEM_OBJECT);
    assert(objectClass != (CORINFO_CLASS_HANDLE) nullptr);
    return objectClass;
}

/*****************************************************************************
 *  "&var" can be used either as TYP_BYREF or TYP_I_IMPL, but we
 *  set its type to TYP_BYREF when we create it. We know if it can be
 *  changed to TYP_I_IMPL only at the point where we use it
 */

/* static */
void Compiler::impBashVarAddrsToI(GenTree* tree1, GenTree* tree2)
{
    if (tree1->OperIs(GT_LCL_ADDR))
    {
        tree1->gtType = TYP_I_IMPL;
    }

    if (tree2 && tree2->OperIs(GT_LCL_ADDR))
    {
        tree2->gtType = TYP_I_IMPL;
    }
}

/*****************************************************************************
 *  TYP_INT and TYP_I_IMPL can be used almost interchangeably, but we want
 *  to make that an explicit cast in our trees, so any implicit casts that
 *  exist in the IL (at least on 64-bit where TYP_I_IMPL != TYP_INT) are
 *  turned into explicit casts here.
 *  We also allow an implicit conversion of a ldnull into a TYP_I_IMPL(0)
 */
GenTree* Compiler::impImplicitIorI4Cast(GenTree* tree, var_types dstTyp, bool zeroExtend)
{
    var_types currType   = genActualType(tree);
    var_types wantedType = genActualType(dstTyp);

    if (wantedType != currType)
    {
        // Automatic upcast for a GT_CNS_INT into TYP_I_IMPL
        if (tree->IsCnsIntOrI() && varTypeIsI(dstTyp))
        {
            if ((currType == TYP_REF) && (tree->AsIntCon()->IconValue() == 0))
            {
                tree->gtType = TYP_I_IMPL;
            }
#ifdef TARGET_64BIT
            else if (currType == TYP_INT)
            {
                tree->gtType = TYP_I_IMPL;
            }
#endif // TARGET_64BIT
        }
#ifdef TARGET_64BIT
        else if (varTypeIsI(wantedType) && (currType == TYP_INT))
        {
            // Note that this allows TYP_INT to be cast to a TYP_I_IMPL when wantedType is a TYP_BYREF or TYP_REF
            tree = gtNewCastNode(TYP_I_IMPL, tree, zeroExtend, TYP_I_IMPL);
        }
        else if ((wantedType == TYP_INT) && varTypeIsI(currType))
        {
            // Note that this allows TYP_BYREF or TYP_REF to be cast to a TYP_INT
            tree = gtNewCastNode(TYP_INT, tree, false, TYP_INT);
        }
#endif // TARGET_64BIT
    }

    return tree;
}

/*****************************************************************************
 *  TYP_FLOAT and TYP_DOUBLE can be used almost interchangeably in most cases,
 *  but we want to make that an explicit cast in our trees, so any implicit casts
 *  that exist in the IL are turned into explicit casts here.
 */

GenTree* Compiler::impImplicitR4orR8Cast(GenTree* tree, var_types dstTyp)
{
    if (varTypeIsFloating(tree) && varTypeIsFloating(dstTyp) && (dstTyp != tree->gtType))
    {
        tree = gtNewCastNode(dstTyp, tree, false, dstTyp);
    }

    return tree;
}

GenTree* Compiler::impTypeIsAssignable(GenTree* typeTo, GenTree* typeFrom)
{
    // Optimize patterns like:
    //
    //   typeof(TTo).IsAssignableFrom(typeof(TTFrom))
    //   valueTypeVar.GetType().IsAssignableFrom(typeof(TTFrom))
    //   typeof(TTFrom).IsAssignableTo(typeof(TTo))
    //   typeof(TTFrom).IsAssignableTo(valueTypeVar.GetType())
    //
    // to true/false

    // make sure both arguments are `typeof()`
    CORINFO_CLASS_HANDLE hClassTo   = NO_CLASS_HANDLE;
    CORINFO_CLASS_HANDLE hClassFrom = NO_CLASS_HANDLE;
    if (gtIsTypeof(typeTo, &hClassTo) && gtIsTypeof(typeFrom, &hClassFrom))
    {
        TypeCompareState castResult = info.compCompHnd->compareTypesForCast(hClassFrom, hClassTo);
        if (castResult == TypeCompareState::May)
        {
            // requires runtime check
            // e.g. __Canon, COMObjects, Nullable
            return nullptr;
        }

        GenTreeIntCon* retNode = gtNewIconNode((castResult == TypeCompareState::Must) ? 1 : 0);
        impPopStack(); // drop both CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE calls
        impPopStack();

        return retNode;
    }

    return nullptr;
}

//------------------------------------------------------------------------
// impGetGenericTypeDefinition: gets the generic type definition from a 'typeof' expression.
//
// Arguments:
//    type - The 'GenTree' node to inspect.
//
// Notes:
//    If successful, this method will call 'impPopStack()' before returning.
//
GenTree* Compiler::impGetGenericTypeDefinition(GenTree* type)
{
    // This intrinsic requires the first arg to be some `typeof()` expression,
    // ie. it applies to cases such as `typeof(...).GetGenericTypeDefinition()`.
    CORINFO_CLASS_HANDLE hClassType = NO_CLASS_HANDLE;
    if (gtIsTypeof(type, &hClassType))
    {
        // Check that the 'typeof()' expression is being used on a type that is in fact generic.
        // If that is not the case, we don't expand the intrinsic. This will end up using
        // the usual Type.GetGenericTypeDefinition() at runtime, which will throw in this case.
        if (info.compCompHnd->getTypeInstantiationArgument(hClassType, 0) != NO_CLASS_HANDLE)
        {
            CORINFO_CLASS_HANDLE hClassResult = info.compCompHnd->getTypeDefinition(hClassType);

            GenTree* handle  = gtNewIconEmbClsHndNode(hClassResult);
            GenTree* retNode = gtNewHelperCallNode(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE, TYP_REF, handle);

            // Drop the typeof(T) node
            impPopStack();

            return retNode;
        }
    }

    return nullptr;
}

typeInfo Compiler::makeTypeInfoForLocal(unsigned lclNum)
{
    LclVarDsc* varDsc = lvaGetDesc(lclNum);

    if (varDsc->TypeIs(TYP_REF))
    {
        return typeInfo(varDsc->lvClassHnd);
    }

    return typeInfo(varDsc->TypeGet());
}

typeInfo Compiler::makeTypeInfo(CorInfoType ciType, CORINFO_CLASS_HANDLE clsHnd)
{
    if (ciType == CORINFO_TYPE_CLASS)
    {
        return typeInfo(clsHnd);
    }

    return typeInfo(JITtype2varType(ciType));
}

typeInfo Compiler::makeTypeInfo(CORINFO_CLASS_HANDLE clsHnd)
{
    assert(clsHnd != NO_CLASS_HANDLE);
    return makeTypeInfo(info.compCompHnd->asCorInfoType(clsHnd), clsHnd);
}

/*****************************************************************************
 *
 *  Check if a TailCall is legal.
 */

bool Compiler::checkTailCallConstraint(OPCODE                  opcode,
                                       CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                       CORINFO_RESOLVED_TOKEN* pConstrainedResolvedToken)
{
    DWORD            mflags;
    CORINFO_SIG_INFO sig;

    CORINFO_METHOD_HANDLE methodHnd       = nullptr;
    CORINFO_CLASS_HANDLE  methodClassHnd  = nullptr;
    unsigned              methodClassFlgs = 0;

    assert(impOpcodeIsCallOpcode(opcode));

    if (compIsForInlining())
    {
        return false;
    }

    // For calli, check that this is not a virtual method.
    if (opcode == CEE_CALLI)
    {
        /* Get the call sig */
        eeGetSig(pResolvedToken->token, pResolvedToken->tokenScope, pResolvedToken->tokenContext, &sig);

        // We don't know the target method, so we have to infer the flags, or
        // assume the worst-case.
        mflags = (sig.callConv & CORINFO_CALLCONV_HASTHIS) ? 0 : CORINFO_FLG_STATIC;
    }
    else
    {
        methodHnd = pResolvedToken->hMethod;

        mflags = info.compCompHnd->getMethodAttribs(methodHnd);

        // In generic code we pair the method handle with its owning class to get the exact method signature.
        methodClassHnd = pResolvedToken->hClass;
        assert(methodClassHnd != NO_CLASS_HANDLE);

        eeGetMethodSig(methodHnd, &sig, methodClassHnd);

        // opcode specific check
        methodClassFlgs = info.compCompHnd->getClassAttribs(methodClassHnd);
    }

    // We must have got the methodClassHnd if opcode is not CEE_CALLI
    assert((methodHnd != nullptr && methodClassHnd != NO_CLASS_HANDLE) || opcode == CEE_CALLI);

    if ((sig.callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_VARARG)
    {
        eeGetCallSiteSig(pResolvedToken->token, pResolvedToken->tokenScope, pResolvedToken->tokenContext, &sig);
    }

    // Check compatibility of the arguments.
    unsigned int            argCount = sig.numArgs;
    CORINFO_ARG_LIST_HANDLE args;
    args = sig.args;
    while (argCount--)
    {
        // For unsafe code, we might have parameters containing pointer to the stack location.
        // Disallow the tailcall for this kind.
        CORINFO_CLASS_HANDLE classHandle;
        CorInfoType          ciType = strip(info.compCompHnd->getArgType(&sig, args, &classHandle));
        if ((ciType == CORINFO_TYPE_PTR) || (ciType == CORINFO_TYPE_BYREF) || (ciType == CORINFO_TYPE_REFANY))
        {
            return false;
        }

        // Check that the argument is not a byref-like for tailcalls.
        if ((ciType == CORINFO_TYPE_VALUECLASS) && eeIsByrefLike(classHandle))
        {
            return false;
        }

        args = info.compCompHnd->getArgNext(args);
    }

    unsigned popCount = sig.totalILArgs();

    // Check for 'this' which is on non-static methods, not called via NEWOBJ
    if ((mflags & CORINFO_FLG_STATIC) == 0)
    {
        if (opcode == CEE_CALLI)
        {
            // For CALLI, we don't know the methodClassHnd. Therefore, let's check the "this" object on the stack.
            if (!impStackTop(popCount).val->TypeIs(TYP_REF))
            {
                return false;
            }
        }
        else
        {
            // Check that the "this" argument is not a byref.
            if (TypeHandleToVarType(methodClassHnd) != TYP_REF)
            {
                return false;
            }
        }
    }

    // Tail calls on constrained calls should be illegal too:
    // when instantiated at a value type, a constrained call may pass the address of a stack allocated value
    if (pConstrainedResolvedToken != nullptr)
    {
        return false;
    }

    // Get the exact view of the signature for an array method
    if (sig.retType != CORINFO_TYPE_VOID)
    {
        if ((methodClassFlgs & CORINFO_FLG_ARRAY) != 0)
        {
            assert(opcode != CEE_CALLI);
            eeGetCallSiteSig(pResolvedToken->token, pResolvedToken->tokenScope, pResolvedToken->tokenContext, &sig);
        }
    }

    var_types calleeRetType = genActualType(JITtype2varType(sig.retType));
    var_types callerRetType = genActualType(JITtype2varType(info.compMethodInfo->args.retType));

    // Normalize TYP_FLOAT to TYP_DOUBLE (it is ok to return one as the other and vice versa).
    calleeRetType = (calleeRetType == TYP_FLOAT) ? TYP_DOUBLE : calleeRetType;
    callerRetType = (callerRetType == TYP_FLOAT) ? TYP_DOUBLE : callerRetType;

    // Make sure the types match.
    if (calleeRetType != callerRetType)
    {
        return false;
    }
    else if ((callerRetType == TYP_STRUCT) && (sig.retTypeClass != info.compMethodInfo->args.retTypeClass))
    {
        return false;
    }

    // For tailcall, stack must be empty.
    if (stackState.esStackDepth != popCount)
    {
        return false;
    }

    return true; // Yes, tailcall is legal
}

GenTree* Compiler::impImportLdvirtftn(GenTree*                thisPtr,
                                      CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                      CORINFO_CALL_INFO*      pCallInfo)
{
    const bool isInterface = (pCallInfo->classFlags & CORINFO_FLG_INTERFACE) == CORINFO_FLG_INTERFACE;

    if ((pCallInfo->methodFlags & CORINFO_FLG_EnC) && !isInterface)
    {
        NO_WAY("Virtual call to a function added via EnC is not supported");
    }

    GenTreeCall* call = nullptr;

    // NativeAOT generic virtual method
    if ((pCallInfo->sig.sigInst.methInstCount != 0) && IsTargetAbi(CORINFO_NATIVEAOT_ABI))
    {
        GenTree* runtimeMethodHandle =
            impLookupToTree(pResolvedToken, &pCallInfo->codePointerLookup, GTF_ICON_METHOD_HDL, pCallInfo->hMethod);
        call = gtNewVirtualFunctionLookupHelperCallNode(CORINFO_HELP_GVMLOOKUP_FOR_SLOT, TYP_I_IMPL, thisPtr,
                                                        runtimeMethodHandle);
    }

#ifdef FEATURE_READYTORUN
    else if (IsAot())
    {
        if (!pCallInfo->exactContextNeedsRuntimeLookup)
        {
            call = gtNewHelperCallNode(CORINFO_HELP_READYTORUN_VIRTUAL_FUNC_PTR, TYP_I_IMPL, thisPtr);
            call->setEntryPoint(pCallInfo->codePointerLookup.constLookup);
        }
        // We need a runtime lookup. NativeAOT has a ReadyToRun helper for that too.
        else if (IsTargetAbi(CORINFO_NATIVEAOT_ABI))
        {
            GenTree* ctxTree = getRuntimeContextTree(pCallInfo->codePointerLookup.lookupKind.runtimeLookupKind);

            call = impReadyToRunHelperToTree(pResolvedToken, CORINFO_HELP_READYTORUN_GENERIC_HANDLE, TYP_I_IMPL,
                                             &pCallInfo->codePointerLookup.lookupKind, ctxTree);
        }
    }
#endif

    if (call == nullptr)
    {
        // Get the exact descriptor for the static callsite
        GenTree* exactTypeDesc = impParentClassTokenToHandle(pResolvedToken);
        if (exactTypeDesc == nullptr)
        {
            assert(compIsForInlining());
            return nullptr;
        }

        GenTree* exactMethodDesc = impTokenToHandle(pResolvedToken);
        if (exactMethodDesc == nullptr)
        {
            assert(compIsForInlining());
            return nullptr;
        }

        // Call helper function.  This gets the target address of the final destination callsite.
        //
        call = gtNewVirtualFunctionLookupHelperCallNode(CORINFO_HELP_VIRTUAL_FUNC_PTR, TYP_I_IMPL, thisPtr,
                                                        exactMethodDesc, exactTypeDesc);
    }

    assert(call != nullptr);

    if (isInterface)
    {
        // Annotate helper so later on if helper result is unconsumed we know it is not sound
        // to optimize the call into a null check.
        //
        call->gtCallMoreFlags |= GTF_CALL_M_LDVIRTFTN_INTERFACE;
    }

    return call;
}

//------------------------------------------------------------------------
// impInlineUnboxNullable: Generate code for unboxing Nullable<T> from an object (obj)
//     We either inline the unbox operation (if profitable) or call the helper.
//     The inline expansion is as follows:
//
//     Nullable<T> result;
//     if (obj == null)
//     {
//         result = default;
//     }
//     else if (obj->pMT == <real-boxed-type>)
//     {
//         result._hasValue = true;
//         result._value = *(T*)(obj + sizeof(void*));
//     }
//     else
//     {
//         result = CORINFO_HELP_UNBOX_NULLABLE(&result, nullableCls, obj);
//     }
//
// Arguments:
//     nullableCls     - class handle representing the Nullable<T> type
//     nullableClsNode - tree node representing the Nullable<T> type (can be a runtime lookup tree)
//     obj             - object to unbox
//
// Return Value:
//     A local node representing the unboxed value (Nullable<T>)
//
GenTree* Compiler::impInlineUnboxNullable(CORINFO_CLASS_HANDLE nullableCls, GenTree* nullableClsNode, GenTree* obj)
{
    assert(info.compCompHnd->isNullableType(nullableCls) == TypeCompareState::Must);

    unsigned resultTmp = lvaGrabTemp(true DEBUGARG("Nullable<T> tmp"));
    lvaSetStruct(resultTmp, nullableCls, false);
    lvaGetDesc(resultTmp)->lvHasLdAddrOp = true;
    GenTreeLclFld* resultAddr            = gtNewLclAddrNode(resultTmp, 0);

    // Check profitability of inlining the unbox operation
    bool shouldExpandInline = !compCurBB->isRunRarely() && opts.OptimizationEnabled() && !eeIsSharedInst(nullableCls);

    // It's less profitable to inline the unbox operation if the underlying type is too large
    CORINFO_CLASS_HANDLE unboxType = NO_CLASS_HANDLE;
    if (shouldExpandInline)
    {
        // The underlying type of the nullable:
        unboxType          = info.compCompHnd->getTypeForBox(nullableCls);
        shouldExpandInline = info.compCompHnd->getClassSize(unboxType) <= getUnrollThreshold(Memcpy);
    }

    if (!shouldExpandInline)
    {
        // No expansion needed, just call the helper
        GenTreeCall* call =
            gtNewHelperCallNode(CORINFO_HELP_UNBOX_NULLABLE, TYP_VOID, resultAddr, nullableClsNode, obj);
        impAppendTree(call, CHECK_SPILL_ALL, impCurStmtDI);
        return gtNewLclvNode(resultTmp, TYP_STRUCT);
    }

    // Clone the object (and spill side effects)
    GenTree* objClone;
    obj = impCloneExpr(obj, &objClone, CHECK_SPILL_ALL, nullptr DEBUGARG("op1 spilled for Nullable unbox"));

    // Unbox the object to the result local:
    //
    //  result._hasValue = true;
    //  result._value = MethodTableLookup(obj);
    //
    CORINFO_FIELD_HANDLE valueFldHnd    = info.compCompHnd->getFieldInClass(nullableCls, 1);
    CORINFO_CLASS_HANDLE valueStructCls = NO_CLASS_HANDLE;
    ClassLayout*         layout         = nullptr;

    CorInfoType corFldType = info.compCompHnd->getFieldType(valueFldHnd, &valueStructCls);
    var_types   valueType  = TypeHandleToVarType(corFldType, valueStructCls, &layout);

    static_assert_no_msg(OFFSETOF__CORINFO_NullableOfT__hasValue == 0);
    unsigned hasValOffset = OFFSETOF__CORINFO_NullableOfT__hasValue;
    unsigned valueOffset  = info.compCompHnd->getFieldOffset(valueFldHnd);

    GenTree* boxedContentAddr =
        gtNewOperNode(GT_ADD, TYP_BYREF, gtCloneExpr(objClone), gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL));
    // Load the boxed content from the object (op1):
    GenTree* boxedContent = gtNewLoadValueNode(valueType, layout, boxedContentAddr);

    // Now do two stores via a comma:
    GenTree* setHasValue = gtNewStoreLclFldNode(resultTmp, TYP_UBYTE, hasValOffset, gtNewIconNode(1));
    GenTree* setValue    = gtNewStoreLclFldNode(resultTmp, valueType, valueOffset, boxedContent);
    GenTree* unboxTree   = gtNewOperNode(GT_COMMA, TYP_VOID, setHasValue, setValue);

    // Fallback helper call
    // TODO: Mark as no-return when appropriate
    GenTreeCall* helperCall =
        gtNewHelperCallNode(CORINFO_HELP_UNBOX_NULLABLE, TYP_VOID, resultAddr, nullableClsNode, gtCloneExpr(objClone));

    // Nested QMARK - "obj->pMT == <boxed-type> ? unboxTree : helperCall"
    assert(unboxType != NO_CLASS_HANDLE);
    GenTree*      unboxTypeNode = gtNewIconEmbClsHndNode(unboxType);
    GenTree*      objMT         = gtNewMethodTableLookup(objClone);
    GenTree*      mtLookupCond  = gtNewOperNode(GT_NE, TYP_INT, objMT, unboxTypeNode);
    GenTreeColon* mtCheckColon  = gtNewColonNode(TYP_VOID, helperCall, unboxTree);
    GenTreeQmark* mtCheckQmark  = gtNewQmarkNode(TYP_VOID, mtLookupCond, mtCheckColon);
    mtCheckQmark->SetThenNodeLikelihood(0);

    // Zero initialize the result in case of "obj == null"
    GenTreeLclVar* zeroInitResultNode = gtNewStoreLclVarNode(resultTmp, gtNewIconNode(0));

    // Root condition - "obj == null ? zeroInitResultNode : mtCheckQmark"
    GenTree*      nullcheck      = gtNewOperNode(GT_NE, TYP_INT, obj, gtNewNull());
    GenTreeColon* nullCheckColon = gtNewColonNode(TYP_VOID, mtCheckQmark, zeroInitResultNode);
    GenTreeQmark* nullCheckQmark = gtNewQmarkNode(TYP_VOID, nullcheck, nullCheckColon);

    // Spill the root QMARK and return the result local
    impAppendTree(nullCheckQmark, CHECK_SPILL_ALL, impCurStmtDI);
    return gtNewLclvNode(resultTmp, TYP_STRUCT);
}

//------------------------------------------------------------------------
// impStoreNullableFields: create a Nullable<T> object and store
//    'hasValue' (always true) and the given value for 'value' field
//
// Arguments:
//    nullableCls - class handle for the Nullable<T> class
//    value       - value to store in 'value' field
//
// Return Value:
//    A local node representing the created Nullable<T> object
//
GenTree* Compiler::impStoreNullableFields(CORINFO_CLASS_HANDLE nullableCls, GenTree* value)
{
    assert(info.compCompHnd->isNullableType(nullableCls) == TypeCompareState::Must);

    CORINFO_FIELD_HANDLE valueFldHnd = info.compCompHnd->getFieldInClass(nullableCls, 1);
    CORINFO_CLASS_HANDLE valueStructCls;
    var_types            valueType = JITtype2varType(info.compCompHnd->getFieldType(valueFldHnd, &valueStructCls));

    // We still make some assumptions about the layout of Nullable<T> in JIT
    static_assert_no_msg(OFFSETOF__CORINFO_NullableOfT__hasValue == 0);
    unsigned hasValOffset = OFFSETOF__CORINFO_NullableOfT__hasValue;
    unsigned valueOffset  = info.compCompHnd->getFieldOffset(valueFldHnd);

    // Define the resulting Nullable<T> local
    unsigned resultTmp = lvaGrabTemp(true DEBUGARG("Nullable<T> tmp"));
    lvaSetStruct(resultTmp, nullableCls, false);

    // Now do two stores:
    GenTree*     hasValueStore = gtNewStoreLclFldNode(resultTmp, TYP_UBYTE, hasValOffset, gtNewIconNode(1));
    ClassLayout* layout        = valueType == TYP_STRUCT ? typGetObjLayout(valueStructCls) : nullptr;
    GenTree*     valueStore    = gtNewStoreLclFldNode(resultTmp, valueType, layout, valueOffset, value);

    // ABI handling for struct values
    if (varTypeIsStruct(valueStore))
    {
        valueStore = impStoreStruct(valueStore, CHECK_SPILL_ALL);
    }

    impAppendTree(hasValueStore, CHECK_SPILL_ALL, impCurStmtDI);
    impAppendTree(valueStore, CHECK_SPILL_ALL, impCurStmtDI);
    return gtNewLclvNode(resultTmp, TYP_STRUCT);
}

//------------------------------------------------------------------------
// impLoadNullableFields: get 'hasValue' and 'value' field loads for Nullable<T> object
//
// Arguments:
//    nullableObj - tree representing the Nullable<T> object
//    nullableCls - class handle for the Nullable<T> class
//    hasValueFld - pointer to store the 'hasValue' field load tree
//    valueFld    - pointer to store the 'value' field load tree
//
void Compiler::impLoadNullableFields(GenTree*             nullableObj,
                                     CORINFO_CLASS_HANDLE nullableCls,
                                     GenTree**            hasValueFld,
                                     GenTree**            valueFld)
{
    assert(info.compCompHnd->isNullableType(nullableCls) == TypeCompareState::Must);

    CORINFO_FIELD_HANDLE valueFldHnd = info.compCompHnd->getFieldInClass(nullableCls, 1);
    CORINFO_CLASS_HANDLE valueStructCls;
    var_types            valueType   = JITtype2varType(info.compCompHnd->getFieldType(valueFldHnd, &valueStructCls));
    ClassLayout*         valueLayout = valueType == TYP_STRUCT ? typGetObjLayout(valueStructCls) : nullptr;

    static_assert_no_msg(OFFSETOF__CORINFO_NullableOfT__hasValue == 0);
    unsigned hasValOffset = OFFSETOF__CORINFO_NullableOfT__hasValue;
    unsigned valueOffset  = info.compCompHnd->getFieldOffset(valueFldHnd);

    unsigned objTmp;
    if (!nullableObj->OperIs(GT_LCL_VAR))
    {
        objTmp = lvaGrabTemp(true DEBUGARG("Nullable<T> tmp"));
        impStoreToTemp(objTmp, nullableObj, CHECK_SPILL_ALL);
    }
    else
    {
        objTmp = nullableObj->AsLclVarCommon()->GetLclNum();
    }

    *hasValueFld = gtNewLclFldNode(objTmp, TYP_UBYTE, hasValOffset);
    *valueFld    = gtNewLclFldNode(objTmp, valueType, valueOffset, valueLayout);
}

//------------------------------------------------------------------------
// impBoxPatternMatch: match and import common box idioms
//
// Arguments:
//   pResolvedToken - resolved token from the box operation
//   codeAddr - position in IL stream after the box instruction
//   codeEndp - end of IL stream
//   opts - dictate pattern matching behavior
//
// Return Value:
//   Number of IL bytes matched and imported, -1 otherwise
//
// Notes:
//   pResolvedToken is known to be a value type; ref type boxing
//   is handled in the CEE_BOX clause.

int Compiler::impBoxPatternMatch(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                 const BYTE*             codeAddr,
                                 const BYTE*             codeEndp,
                                 BoxPatterns             opts)
{
    if (codeAddr >= codeEndp)
    {
        return -1;
    }

    switch (codeAddr[0])
    {
        case CEE_UNBOX_ANY:
            // box + unbox.any
            if (codeAddr + 1 + sizeof(mdToken) <= codeEndp)
            {
                if (opts == BoxPatterns::MakeInlineObservation)
                {
                    compInlineResult->Note(InlineObservation::CALLEE_FOLDABLE_BOX);
                    return 1 + sizeof(mdToken);
                }

                CORINFO_RESOLVED_TOKEN unboxResolvedToken;

                impResolveToken(codeAddr + 1, &unboxResolvedToken, CORINFO_TOKENKIND_Class);

                // See if the resolved tokens describe types that are equal.
                const TypeCompareState compare =
                    info.compCompHnd->compareTypesForEquality(unboxResolvedToken.hClass, pResolvedToken->hClass);

                bool optimize = false;

                // If so, box/unbox.any is a nop.
                if (compare == TypeCompareState::Must)
                {
                    optimize = true;
                }
                else if (compare == TypeCompareState::MustNot)
                {
                    // An attempt to catch cases where we mix enums and primitives, e.g.:
                    //   (IntEnum)(object)myInt
                    //   (byte)(object)myByteEnum
                    //
                    CorInfoType typ = info.compCompHnd->getTypeForPrimitiveValueClass(unboxResolvedToken.hClass);
                    if ((typ >= CORINFO_TYPE_BYTE) && (typ <= CORINFO_TYPE_ULONG) &&
                        (info.compCompHnd->getTypeForPrimitiveValueClass(pResolvedToken->hClass) == typ))
                    {
                        optimize = true;
                    }
                    //
                    // Also, try to optimize (T)(object)nullableT
                    //
                    else if (!eeIsSharedInst(unboxResolvedToken.hClass) &&
                             (info.compCompHnd->isNullableType(pResolvedToken->hClass) == TypeCompareState::Must) &&
                             (info.compCompHnd->getTypeForBox(pResolvedToken->hClass) == unboxResolvedToken.hClass))
                    {
                        GenTree* hasValueFldTree;
                        GenTree* valueFldTree;
                        impLoadNullableFields(impPopStack().val, pResolvedToken->hClass, &hasValueFldTree,
                                              &valueFldTree);

                        // Push "hasValue == 0 ? throw new NullReferenceException() : NOP" qmark
                        GenTree*      fallback = gtNewHelperCallNode(CORINFO_HELP_THROWNULLREF, TYP_VOID);
                        GenTree*      cond     = gtNewOperNode(GT_EQ, TYP_INT, hasValueFldTree, gtNewIconNode(0));
                        GenTreeColon* colon    = gtNewColonNode(TYP_VOID, fallback, gtNewNothingNode());
                        GenTree*      qmark    = gtNewQmarkNode(TYP_VOID, cond, colon);
                        impAppendTree(qmark, CHECK_SPILL_ALL, impCurStmtDI);

                        // Now push the value field
                        impPushOnStack(valueFldTree, typeInfo(valueFldTree->TypeGet()));
                        optimize = true;
                    }
                    //
                    // Vice versa, try to optimize (T?)(object)nonNullableT
                    //
                    else if (!eeIsSharedInst(pResolvedToken->hClass) &&
                             (info.compCompHnd->isNullableType(unboxResolvedToken.hClass) == TypeCompareState::Must) &&
                             (info.compCompHnd->getTypeForBox(unboxResolvedToken.hClass) == pResolvedToken->hClass))
                    {
                        GenTree* result = impStoreNullableFields(unboxResolvedToken.hClass, impPopStack().val);
                        impPushOnStack(result, typeInfo(result->TypeGet()));
                        optimize = true;
                    }
                }

                if (optimize)
                {
                    JITDUMP("\n Importing BOX; UNBOX.ANY as NOP\n");
                    // Skip the next unbox.any instruction
                    return 1 + sizeof(mdToken);
                }
            }
            break;

        case CEE_BRTRUE:
        case CEE_BRTRUE_S:
        case CEE_BRFALSE:
        case CEE_BRFALSE_S:
            // box + br_true/false
            if ((codeAddr + ((codeAddr[0] >= CEE_BRFALSE) ? 5 : 2)) <= codeEndp)
            {
                if (opts == BoxPatterns::MakeInlineObservation)
                {
                    compInlineResult->Note(InlineObservation::CALLEE_FOLDABLE_BOX);
                    return 0;
                }

                if ((opts == BoxPatterns::IsByRefLike) ||
                    (info.compCompHnd->getBoxHelper(pResolvedToken->hClass) == CORINFO_HELP_BOX))
                {
                    JITDUMP("\n Importing BOX; BR_TRUE/FALSE as constant\n")

                    impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("spilling side-effects"));
                    impPopStack();
                    impPushOnStack(gtNewTrue(), typeInfo(TYP_INT));
                    return 0;
                }
            }
            break;

        case CEE_ISINST:
            if (codeAddr + 1 + sizeof(mdToken) + 1 <= codeEndp)
            {
                // First, let's see if we can fold BOX+ISINST to just null if ISINST is known to return null
                // for the given argument. Don't make inline observations for this case.
                if ((opts == BoxPatterns::None) &&
                    (info.compCompHnd->getBoxHelper(pResolvedToken->hClass) == CORINFO_HELP_BOX))
                {
                    CORINFO_RESOLVED_TOKEN isInstTok;
                    impResolveToken(codeAddr + 1, &isInstTok, CORINFO_TOKENKIND_Casting);
                    if (info.compCompHnd->compareTypesForCast(pResolvedToken->hClass, isInstTok.hClass) ==
                        TypeCompareState::MustNot)
                    {
                        JITDUMP("\n Importing BOX; ISINST; as null\n");

                        impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("spilling side-effects"));
                        impPopStack();
                        impPushOnStack(gtNewNull(), typeInfo(TYP_REF));
                        return 1 + sizeof(mdToken);
                    }
                }

                const BYTE*  nextCodeAddr = codeAddr + 1 + sizeof(mdToken);
                const OPCODE nextOpcode   = impGetNonPrefixOpcode(nextCodeAddr, codeEndp);
                switch (nextOpcode)
                {
                    // box + isinst + br_true/false
                    case CEE_BRTRUE:
                    case CEE_BRTRUE_S:
                    case CEE_BRFALSE:
                    case CEE_BRFALSE_S:
                    case CEE_LDNULL:
                    {
                        // "ldnull + cgt_un" is used when BOX+ISINST is not fed into a branch, e.g.:
                        //
                        //   if (obj is string) {        <--- box + isinst + br_true
                        //
                        //   bool b = obj is string;     <--- box + isinst + ldnull + cgt_un
                        //
                        //   bool b = obj is not string; <--- box + isinst + ldnull + cgt_un + ldc.i4.0 + ceq
                        //

                        // For br_true/false, we'll only replace "box + isinst" to a boolean
                        int returnToken = 1 + sizeof(mdToken);
                        if (nextOpcode == CEE_LDNULL)
                        {
                            // for ldnull case, we'll replace the whole "box + isinst + ldnull + cgt_un" sequence
                            returnToken = 4 + sizeof(mdToken);
                            if ((opts == BoxPatterns::IsByRefLike) ||
                                (impGetNonPrefixOpcode(nextCodeAddr + 1, codeEndp) != CEE_CGT_UN))
                            {
                                break;
                            }
                        }

                        if (opts == BoxPatterns::MakeInlineObservation)
                        {
                            compInlineResult->Note(InlineObservation::CALLEE_FOLDABLE_BOX);
                            return returnToken;
                        }

                        CorInfoHelpFunc foldAsHelper;
                        if (opts == BoxPatterns::IsByRefLike)
                        {
                            // Treat ByRefLike types as if they were regular boxing operations
                            // so they can be elided.
                            foldAsHelper = CORINFO_HELP_BOX;
                        }
                        else
                        {
                            foldAsHelper = info.compCompHnd->getBoxHelper(pResolvedToken->hClass);
                        }

                        if (foldAsHelper == CORINFO_HELP_BOX)
                        {
                            CORINFO_RESOLVED_TOKEN isInstResolvedToken;

                            impResolveToken(codeAddr + 1, &isInstResolvedToken, CORINFO_TOKENKIND_Casting);

                            TypeCompareState castResult =
                                info.compCompHnd->compareTypesForCast(pResolvedToken->hClass,
                                                                      isInstResolvedToken.hClass);

                            if (castResult != TypeCompareState::May)
                            {
                                JITDUMP("\n Importing BOX; ISINST; BR_TRUE/FALSE as constant\n");

                                impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("spilling side-effects"));
                                impPopStack();
                                impPushOnStack(gtNewIconNode((castResult == TypeCompareState::Must) ? 1 : 0),
                                               typeInfo(TYP_INT));
                                return returnToken;
                            }
                        }
                        else if ((foldAsHelper == CORINFO_HELP_BOX_NULLABLE) &&
                                 ((impStackTop().val->gtFlags & GTF_SIDE_EFFECT) == 0))
                        {
                            // For nullable we're going to fold it to "ldfld hasValue + brtrue/brfalse" or
                            // "ldc.i4.0 + brtrue/brfalse" in case if the underlying type is not castable to
                            // the target type.
                            CORINFO_RESOLVED_TOKEN isInstResolvedToken;
                            impResolveToken(codeAddr + 1, &isInstResolvedToken, CORINFO_TOKENKIND_Casting);

                            CORINFO_CLASS_HANDLE nullableCls   = pResolvedToken->hClass;
                            CORINFO_CLASS_HANDLE underlyingCls = info.compCompHnd->getTypeForBox(nullableCls);

                            TypeCompareState castResult =
                                info.compCompHnd->compareTypesForCast(underlyingCls, isInstResolvedToken.hClass);

                            if (castResult == TypeCompareState::Must)
                            {
                                GenTree* objToBox = impPopStack().val;

                                // Spill struct to get its address (to access hasValue field)
                                // TODO-Bug?: verify if flags matter here
                                GenTreeFlags indirFlags = GTF_EMPTY;
                                objToBox                = impGetNodeAddr(objToBox, CHECK_SPILL_ALL, &indirFlags);

                                static_assert_no_msg(OFFSETOF__CORINFO_NullableOfT__hasValue == 0);
                                impPushOnStack(gtNewIndir(TYP_UBYTE, objToBox), typeInfo(TYP_INT));

                                JITDUMP("\n Importing BOX; ISINST; BR_TRUE/FALSE as nullableVT.hasValue\n");
                                return returnToken;
                            }
                            else if (castResult == TypeCompareState::MustNot)
                            {
                                impPopStack();
                                impPushOnStack(gtNewIconNode(0), typeInfo(TYP_INT));
                                JITDUMP("\n Importing BOX; ISINST; BR_TRUE/FALSE as constant (false)\n");
                                return returnToken;
                            }
                        }
                    }
                    break;

                    // box + isinst + unbox.any
                    case CEE_UNBOX_ANY:
                    {
                        if (opts == BoxPatterns::MakeInlineObservation)
                        {
                            compInlineResult->Note(InlineObservation::CALLEE_FOLDABLE_BOX);
                            return 2 + sizeof(mdToken) * 2;
                        }

                        // See if the resolved tokens in box, isinst and unbox.any describe types that are equal.
                        CORINFO_RESOLVED_TOKEN isinstResolvedToken = {};
                        impResolveToken(codeAddr + 1, &isinstResolvedToken, CORINFO_TOKENKIND_Class);

                        if (info.compCompHnd->compareTypesForEquality(isinstResolvedToken.hClass,
                                                                      pResolvedToken->hClass) == TypeCompareState::Must)
                        {
                            CORINFO_RESOLVED_TOKEN unboxResolvedToken = {};
                            impResolveToken(nextCodeAddr + 1, &unboxResolvedToken, CORINFO_TOKENKIND_Class);

                            // If so, box + isinst + unbox.any is a nop.
                            if (info.compCompHnd->compareTypesForEquality(unboxResolvedToken.hClass,
                                                                          pResolvedToken->hClass) ==
                                TypeCompareState::Must)
                            {
                                JITDUMP("\n Importing BOX; ISINST, UNBOX.ANY as NOP\n");
                                return 2 + sizeof(mdToken) * 2;
                            }
                        }
                    }
                    break;

                    default:
                        break;
                }
            }
            break;

        default:
            break;
    }

    return -1;
}

//------------------------------------------------------------------------
// impImportAndPushBox: build and import a value-type box
//
// Arguments:
//   pResolvedToken - resolved token from the box operation
//
// Return Value:
//   None.
//
// Side Effects:
//   The value to be boxed is popped from the stack, and a tree for
//   the boxed value is pushed. This method may create upstream
//   statements, spill side effecting trees, and create new temps.
//
//   If importing an inlinee, we may also discover the inline must
//   fail. If so there is no new value pushed on the stack. Callers
//   should use CompDoNotInline after calling this method to see if
//   ongoing importation should be aborted.
//
// Notes:
//   Boxing of ref classes results in the same value as the value on
//   the top of the stack, so is handled inline in impImportBlockCode
//   for the CEE_BOX case. Only value or primitive type boxes make it
//   here.
//
//   Boxing for nullable types is done via a helper call; boxing
//   of other value types is expanded inline or handled via helper
//   call, depending on the jit's codegen mode.
//
//   When the jit is operating in size and time constrained modes,
//   using a helper call here can save jit time and code size. But it
//   also may inhibit cleanup optimizations that could have also had a
//   even greater benefit effect on code size and jit time. An optimal
//   strategy may need to peek ahead and see if it is easy to tell how
//   the box is being used. For now, we defer.

void Compiler::impImportAndPushBox(CORINFO_RESOLVED_TOKEN* pResolvedToken)
{
    // Spill any special side effects
    impSpillSpecialSideEff();

    // Get get the expression to box from the stack.
    GenTree*   op1       = nullptr;
    GenTree*   op2       = nullptr;
    StackEntry se        = impPopStack();
    GenTree*   exprToBox = se.val;

    // Look at what helper we should use.
    CorInfoHelpFunc boxHelper = info.compCompHnd->getBoxHelper(pResolvedToken->hClass);

    // Determine what expansion to prefer.
    //
    // In size/time/debuggable constrained modes, the helper call
    // expansion for box is generally smaller and is preferred, unless
    // the value to box is a struct that comes from a call. In that
    // case the call can construct its return value directly into the
    // box payload, saving possibly some up-front zeroing.
    //
    // Currently primitive type boxes always get inline expanded. We may
    // want to do the same for small structs if they don't come from
    // calls and don't have GC pointers, since explicitly copying such
    // structs is cheap.
    JITDUMP("\nCompiler::impImportAndPushBox -- handling BOX(value class) via");
    bool canExpandInline = (boxHelper == CORINFO_HELP_BOX);
    bool optForSize      = !exprToBox->IsCall() && varTypeIsStruct(exprToBox) && opts.OptimizationDisabled();
    bool expandInline    = canExpandInline && !optForSize;

    if (expandInline)
    {
        JITDUMP(" inline allocate/copy sequence\n");

        // we are doing 'normal' boxing.  This means that we can inline the box operation
        // Box(expr) gets morphed into
        // temp = new(clsHnd)
        // cpobj(temp+4, expr, clsHnd)
        // push temp
        // The code paths differ slightly below for structs and primitives because
        // "cpobj" differs in these cases.  In one case you get
        //    impStoreStructPtr(temp+4, expr, clsHnd)
        // and the other you get
        //    *(temp+4) = expr

        // For minopts/debug code, try and minimize the total number
        // of box temps by reusing an existing temp when possible. However,
        bool shareBoxedTemps = opts.OptimizationDisabled();

        // Avoid sharing in some tier 0 cases to, potentially, avoid boxing in Enum.HasFlag.
        if (shareBoxedTemps && varTypeIsIntegral(exprToBox) && !lvaHaveManyLocals() &&
            (info.compCompHnd->isEnum(pResolvedToken->hClass, nullptr) != TypeCompareState::Must))
        {
            shareBoxedTemps = false;
        }

        if (shareBoxedTemps)
        {
            // For minopts/debug code, try and minimize the total number
            // of box temps by reusing an existing temp when possible.
            if (impBoxTempInUse || impBoxTemp == BAD_VAR_NUM)
            {
                impBoxTemp = lvaGrabTemp(true DEBUGARG("Reusable Box Helper"));
            }
        }
        else
        {
            // When optimizing, use a new temp for each box operation
            // since we then know the exact class of the box temp.
            impBoxTemp                       = lvaGrabTemp(true DEBUGARG("Single-def Box Helper"));
            lvaTable[impBoxTemp].lvType      = TYP_REF;
            lvaTable[impBoxTemp].lvSingleDef = 1;
            JITDUMP("Marking V%02u as a single def local\n", impBoxTemp);
            const bool isExact = true;
            lvaSetClass(impBoxTemp, pResolvedToken->hClass, isExact);
        }

        // needs to stay in use until this box expression is appended
        // some other node.  We approximate this by keeping it alive until
        // the opcode stack becomes empty
        impBoxTempInUse = true;

        // Remember the current last statement in case we need to move
        // a range of statements to ensure the box temp is initialized
        // before it's used.
        //
        Statement* const cursor = impLastStmt;

        const bool useParent = false;
        op1                  = gtNewAllocObjNode(pResolvedToken, info.compMethodHnd, useParent);
        if (op1 == nullptr)
        {
            // If we fail to create the newobj node, we must be inlining
            // and have run across a type we can't describe.
            //
            assert(compDonotInline());
            return;
        }

        // Remember that this basic block contains 'new' of an object,
        // and so does this method
        //
        compCurBB->SetFlags(BBF_HAS_NEWOBJ);
        optMethodFlags |= OMF_HAS_NEWOBJ;

        // Assign the boxed object to the box temp.
        //
        GenTree*   allocBoxStore = gtNewTempStore(impBoxTemp, op1);
        Statement* allocBoxStmt  = impAppendTree(allocBoxStore, CHECK_SPILL_NONE, impCurStmtDI);

        // If the exprToBox is a call that returns its value via a ret buf arg,
        // move the store statement(s) before the call (which must be a top level tree).
        //
        // We do this because impStoreStructPtr (invoked below) will
        // back-substitute into a call when it sees a GT_RET_EXPR and the call
        // has a hidden buffer pointer, So we need to reorder things to avoid
        // creating out-of-sequence IR.
        //
        if (varTypeIsStruct(exprToBox) && exprToBox->OperIs(GT_RET_EXPR))
        {
            GenTreeCall* const call = exprToBox->AsRetExpr()->gtInlineCandidate->AsCall();

            // If the call was flagged for possible enumerator cloning, flag the allocation as well.
            //
            if (compIsForInlining() && hasImpEnumeratorGdvLocalMap())
            {
                NodeToUnsignedMap* const map           = getImpEnumeratorGdvLocalMap();
                unsigned                 enumeratorLcl = BAD_VAR_NUM;
                GenTreeCall* const       call          = impInlineInfo->iciCall;
                if (map->Lookup(call, &enumeratorLcl))
                {
                    JITDUMP("Flagging [%06u] for enumerator cloning via V%02u\n", dspTreeID(op1), enumeratorLcl);
                    map->Remove(call);
                    map->Set(op1, enumeratorLcl);
                }
            }

            if (call->ShouldHaveRetBufArg())
            {
                JITDUMP("Must insert newobj stmts for box before call [%06u]\n", dspTreeID(call));

                // Walk back through the statements in this block, looking for the one
                // that has this call as the root node.
                //
                // Because gtNewTempStore (above) may have added statements that
                // feed into the actual store we need to move this set of added
                // statements as a group.
                //
                // Note boxed allocations are side-effect free (no com or finalizer) so
                // our only worries here are (correctness) not overlapping the box temp
                // lifetime and (perf) stretching the temp lifetime across the inlinee
                // body.
                //
                // Since this is an inline candidate, we must be optimizing, and so we have
                // a unique box temp per call. So no worries about overlap.
                //
                assert(!opts.OptimizationDisabled());

                // Lifetime stretching could addressed with some extra cleverness--sinking
                // the allocation back down to just before the copy, once we figure out
                // where the copy is. We defer for now.
                //
                Statement* insertBeforeStmt = cursor;
                noway_assert(insertBeforeStmt != nullptr);

                while (true)
                {
                    if (insertBeforeStmt->GetRootNode() == call)
                    {
                        break;
                    }

                    // If we've searched all the statements in the block and failed to
                    // find the call, then something's wrong.
                    //
                    noway_assert(insertBeforeStmt != impStmtList);

                    insertBeforeStmt = insertBeforeStmt->GetPrevStmt();
                }

                // Found the call. Move the statements comprising the store.
                //
                JITDUMP("Moving " FMT_STMT "..." FMT_STMT " before " FMT_STMT "\n", cursor->GetNextStmt()->GetID(),
                        allocBoxStmt->GetID(), insertBeforeStmt->GetID());
                assert(allocBoxStmt == impLastStmt);
                do
                {
                    Statement* movingStmt = impExtractLastStmt();
                    impInsertStmtBefore(movingStmt, insertBeforeStmt);
                    insertBeforeStmt = movingStmt;
                } while (impLastStmt != cursor);
            }
        }

        // Create a pointer to the box payload in op1.
        //
        op1 = gtNewLclvNode(impBoxTemp, TYP_REF);
        op2 = gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL);
        op1 = gtNewOperNode(GT_ADD, TYP_BYREF, op1, op2);

        // Copy from the exprToBox to the box payload.
        //
        if (varTypeIsStruct(exprToBox))
        {
            op1 = impStoreStructPtr(op1, exprToBox, CHECK_SPILL_ALL);
        }
        else
        {
            var_types lclTyp = exprToBox->TypeGet();
            if (lclTyp == TYP_BYREF)
            {
                lclTyp = TYP_I_IMPL;
            }
            CorInfoType jitType = info.compCompHnd->asCorInfoType(pResolvedToken->hClass);
            if (impIsPrimitive(jitType))
            {
                lclTyp = JITtype2varType(jitType);
            }

            var_types srcTyp = exprToBox->TypeGet();
            var_types dstTyp = lclTyp;

            // We allow float <-> double mismatches and implicit truncation for small types.
            assert((genActualType(srcTyp) == genActualType(dstTyp)) ||
                   (varTypeIsFloating(srcTyp) == varTypeIsFloating(dstTyp)));

            // Note regarding small types.
            // We are going to store to the box here via an indirection, so the cast added below is
            // redundant, since the store has an implicit truncation semantic. The reason we still
            // add this cast is so that the code which deals with GT_BOX optimizations does not have
            // to account for this implicit truncation (e. g. understand that BOX<byte>(0xFF + 1) is
            // actually BOX<byte>(0) or deal with signedness mismatch and other GT_CAST complexities).
            if (srcTyp != dstTyp)
            {
                exprToBox = gtNewCastNode(genActualType(dstTyp), exprToBox, false, dstTyp);
            }

            op1 = gtNewStoreIndNode(dstTyp, op1, exprToBox, GTF_IND_NONFAULTING);
        }

        // Spill eval stack to flush out any pending side effects.
        impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("impImportAndPushBox"));

        // Set up this copy as a second store.
        Statement* copyStmt = impAppendTree(op1, CHECK_SPILL_NONE, impCurStmtDI);

        op1 = gtNewLclvNode(impBoxTemp, TYP_REF);

        // Record that this is a "box" node and keep track of the matching parts.
        op1 = new (this, GT_BOX) GenTreeBox(TYP_REF, op1, allocBoxStmt, copyStmt);

        // If it is a value class, mark the "box" node.  We can use this information
        // to optimise several cases:
        //    "box(x) == null" --> false
        //    "(box(x)).CallAnInterfaceMethod(...)" --> "(&x).CallAValueTypeMethod"
        //    "(box(x)).CallAnObjectMethod(...)" --> "(&x).CallAValueTypeMethod"

        op1->gtFlags |= GTF_BOX_VALUE;
        assert(op1->IsBoxedValue() && allocBoxStore->OperIs(GT_STORE_LCL_VAR));
    }
    else
    {
        // Don't optimize, just call the helper and be done with it.
        JITDUMP(" helper call because: %s\n", canExpandInline ? "optimizing for size" : "nullable");

        // Ensure that the value class is restored
        op2 = impTokenToHandle(pResolvedToken, nullptr, true /* mustRestoreHandle */);
        if (op2 == nullptr)
        {
            // We must be backing out of an inline.
            assert(compDonotInline());
            return;
        }

        // TODO-Bug?: verify if flags matter here
        GenTreeFlags indirFlags = GTF_EMPTY;
        op1 = gtNewHelperCallNode(boxHelper, TYP_REF, op2, impGetNodeAddr(exprToBox, CHECK_SPILL_ALL, &indirFlags));
    }

    /* Push the result back on the stack, */
    /* even if clsHnd is a value class we want the TYP_REF */
    typeInfo tiRetVal = typeInfo(info.compCompHnd->getTypeForBox(pResolvedToken->hClass));
    impPushOnStack(op1, tiRetVal);
}

//------------------------------------------------------------------------
// impImportNewObjArray: Build and import `new` of multi-dimensional array
//
// Arguments:
//    pResolvedToken - The CORINFO_RESOLVED_TOKEN that has been initialized
//                     by a call to CEEInfo::resolveToken().
//    pCallInfo - The CORINFO_CALL_INFO that has been initialized
//                by a call to CEEInfo::getCallInfo().
//
// Assumptions:
//    The multi-dimensional array constructor arguments (array dimensions) are
//    pushed on the IL stack on entry to this method.
//
// Notes:
//    Multi-dimensional array constructors are imported as calls to a JIT
//    helper, not as regular calls.
//
void Compiler::impImportNewObjArray(CORINFO_RESOLVED_TOKEN* pResolvedToken, CORINFO_CALL_INFO* pCallInfo)
{
    GenTree* classHandle = impParentClassTokenToHandle(pResolvedToken);
    if (classHandle == nullptr)
    { // compDonotInline()
        return;
    }

    assert(pCallInfo->sig.numArgs);

    GenTree* node;

    unsigned dimensionsSize = pCallInfo->sig.numArgs * sizeof(INT32);
    // Reuse the temp used to pass the array dimensions to avoid bloating
    // the stack frame in case there are multiple calls to multi-dim array
    // constructors within a single method.
    if (lvaNewObjArrayArgs == BAD_VAR_NUM)
    {
        lvaNewObjArrayArgs = lvaGrabTemp(false DEBUGARG("NewObjArrayArgs"));
        lvaSetStruct(lvaNewObjArrayArgs, typGetBlkLayout(dimensionsSize), false);
    }

    // Increase size of lvaNewObjArrayArgs to be the largest size needed to hold 'numArgs' integers
    // for our call to CORINFO_HELP_NEW_MDARR.
    if (dimensionsSize > lvaTable[lvaNewObjArrayArgs].lvExactSize())
    {
        lvaTable[lvaNewObjArrayArgs].GrowBlockLayout(typGetBlkLayout(dimensionsSize));
    }

    // The side-effects may include allocation of more multi-dimensional arrays. Spill all side-effects
    // to ensure that the shared lvaNewObjArrayArgs local variable is only ever used to pass arguments
    // to one allocation at a time.
    impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("impImportNewObjArray"));

    //
    // The arguments of the CORINFO_HELP_NEW_MDARR helper are:
    //  - Array class handle
    //  - Number of dimension arguments
    //  - Pointer to block of int32 dimensions: address of lvaNewObjArrayArgs temp.
    //

    node = gtNewLclVarAddrNode(lvaNewObjArrayArgs);

    // Pop dimension arguments from the stack one at a time and store it
    // into lvaNewObjArrayArgs temp.
    for (int i = pCallInfo->sig.numArgs - 1; i >= 0; i--)
    {
        GenTree* arg   = impImplicitIorI4Cast(impPopStack().val, TYP_INT);
        GenTree* store = gtNewStoreLclFldNode(lvaNewObjArrayArgs, TYP_INT, sizeof(INT32) * i, arg);
        node           = gtNewOperNode(GT_COMMA, node->TypeGet(), store, node);
    }

    CorInfoHelpFunc helper = info.compCompHnd->getArrayRank(pResolvedToken->hClass) == 1 ? CORINFO_HELP_NEW_MDARR_RARE
                                                                                         : CORINFO_HELP_NEW_MDARR;

    node = gtNewHelperCallNode(helper, TYP_REF, classHandle, gtNewIconNode(pCallInfo->sig.numArgs), node);

    node->AsCall()->compileTimeHelperArgumentHandle = (CORINFO_GENERIC_HANDLE)pResolvedToken->hClass;

    // Remember that this function contains 'new' of a MD array.
    optMethodFlags |= OMF_HAS_MDNEWARRAY;

    impPushOnStack(node, typeInfo(pResolvedToken->hClass));
}

//------------------------------------------------------------------------
// impInitClass: Build a node to initialize the class before accessing the
//               field if necessary
//
// Arguments:
//    pResolvedToken - The CORINFO_RESOLVED_TOKEN that has been initialized
//                     by a call to CEEInfo::resolveToken().
//
// Return Value: If needed, a pointer to the node that will perform the class
//               initializtion.  Otherwise, nullptr.
//

GenTree* Compiler::impInitClass(CORINFO_RESOLVED_TOKEN* pResolvedToken)
{
    CorInfoInitClassResult initClassResult =
        info.compCompHnd->initClass(pResolvedToken->hField, info.compMethodHnd, impTokenLookupContextHandle);

    if ((initClassResult & CORINFO_INITCLASS_USE_HELPER) == 0)
    {
        return nullptr;
    }
    bool runtimeLookup;

    GenTree* node = impParentClassTokenToHandle(pResolvedToken, &runtimeLookup);

    if (node == nullptr)
    {
        assert(compDonotInline());
        return nullptr;
    }

    if (runtimeLookup)
    {
        node = gtNewHelperCallNode(CORINFO_HELP_INITCLASS, TYP_VOID, node);
    }
    else
    {
        // Call the shared non gc static helper, as its the fastest
        node = fgGetSharedCCtor(pResolvedToken->hClass);
    }

    return node;
}

//------------------------------------------------------------------------
// impImportStaticReadOnlyField: Tries to import 'static readonly' field
//    as a constant if the host type is statically initialized.
//
// Arguments:
//    field    - 'static readonly' field
//    ownerCls - class handle of the type the given field defined in
//
// Return Value:
//    The tree representing the constant value of the statically initialized
//    readonly tree.
//
GenTree* Compiler::impImportStaticReadOnlyField(CORINFO_FIELD_HANDLE field, CORINFO_CLASS_HANDLE ownerCls)
{
    if (!opts.OptimizationEnabled())
    {
        return nullptr;
    }

    JITDUMP("\nChecking if we can import 'static readonly' as a jit-time constant... ")

    CORINFO_CLASS_HANDLE fieldClsHnd;
    var_types            fieldType = JITtype2varType(info.compCompHnd->getFieldType(field, &fieldClsHnd, ownerCls));

    const int bufferSize         = sizeof(uint64_t);
    uint8_t   buffer[bufferSize] = {0};
    if (varTypeIsIntegral(fieldType) || varTypeIsFloating(fieldType) || (fieldType == TYP_REF))
    {
        assert(bufferSize >= genTypeSize(fieldType));
        if (info.compCompHnd->getStaticFieldContent(field, buffer, genTypeSize(fieldType)))
        {
            GenTree* cnsValue = gtNewGenericCon(fieldType, buffer);
            if (cnsValue != nullptr)
            {
                JITDUMP("... success! The value is:\n");
                DISPTREE(cnsValue);
                return cnsValue;
            }
        }
    }
    else if (fieldType == TYP_STRUCT)
    {
        unsigned totalSize = info.compCompHnd->getClassSize(fieldClsHnd);
        unsigned fieldsCnt = info.compCompHnd->getClassNumInstanceFields(fieldClsHnd);

        // For large structs we only want to handle "initialized with zero" case
        // e.g. Guid.Empty and decimal.Zero static readonly fields.
        if ((totalSize > TARGET_POINTER_SIZE) || (fieldsCnt != 1))
        {
            JITDUMP("checking if we can do anything for a large struct ...");
            const int MaxStructSize = 64;
            if ((totalSize == 0) || (totalSize > MaxStructSize))
            {
                // Limit to 64 bytes for better throughput
                JITDUMP("struct is larger than 64 bytes - bail out.");
                return nullptr;
            }

            uint8_t buffer[MaxStructSize] = {0};
            if (info.compCompHnd->getStaticFieldContent(field, buffer, totalSize))
            {
#ifdef FEATURE_SIMD
                // First, let's check whether field is a SIMD vector and import it as GT_CNS_VEC
                int simdWidth = getSIMDTypeSizeInBytes(fieldClsHnd);
                if (simdWidth > 0)
                {
                    assert((totalSize <= 64) && (totalSize <= MaxStructSize));
                    var_types simdType = getSIMDTypeForSize(simdWidth);

                    bool hwAccelerated = true;
#ifdef TARGET_XARCH
                    if (simdType == TYP_SIMD64)
                    {
                        hwAccelerated = compOpportunisticallyDependsOn(InstructionSet_AVX512);
                    }
                    else if (simdType == TYP_SIMD32)
                    {
                        hwAccelerated = compOpportunisticallyDependsOn(InstructionSet_AVX);
                    }
                    else
#endif // TARGET_XARCH
                    {
                        // SIMD8, SIMD12, SIMD16 are covered by baseline ISA requirement
                        assert((simdType == TYP_SIMD8) || (simdType == TYP_SIMD12) || (simdType == TYP_SIMD16));
                    }

                    if (hwAccelerated)
                    {
                        GenTreeVecCon* vec = gtNewVconNode(simdType);
                        memcpy(&vec->gtSimdVal, buffer, totalSize);
                        return vec;
                    }
                }
#endif // FEATURE_SIMD

                for (unsigned i = 0; i < totalSize; i++)
                {
                    if (buffer[i] != 0)
                    {
                        // Value is not all zeroes - bail out.
                        // Although, We might eventually support that too.
                        JITDUMP("value is not all zeros - bail out.");
                        return nullptr;
                    }
                }

                JITDUMP("Success! Optimizing to STORE_LCL_VAR<struct>(0).");
                unsigned structTempNum = lvaGrabTemp(true DEBUGARG("folding static readonly field empty struct"));
                lvaSetStruct(structTempNum, fieldClsHnd, false);

                impStoreToTemp(structTempNum, gtNewIconNode(0), CHECK_SPILL_NONE);

                return gtNewLclVarNode(structTempNum);
            }

            JITDUMP("getStaticFieldContent returned false - bail out.");
            return nullptr;
        }

        // Only single-field structs are supported here to avoid potential regressions where
        // Metadata-driven struct promotion leads to regressions.

        CORINFO_FIELD_HANDLE innerField = info.compCompHnd->getFieldInClass(fieldClsHnd, 0);
        CORINFO_CLASS_HANDLE innerFieldClsHnd;
        var_types            fieldVarType =
            JITtype2varType(info.compCompHnd->getFieldType(innerField, &innerFieldClsHnd, fieldClsHnd));

        // Technically, we can support frozen gc refs here and maybe floating point in future
        if (!varTypeIsIntegral(fieldVarType))
        {
            JITDUMP("struct has non-primitive fields - bail out.");
            return nullptr;
        }

        unsigned fldOffset = info.compCompHnd->getFieldOffset(innerField);

        if ((fldOffset != 0) || (totalSize != genTypeSize(fieldVarType)) || (totalSize == 0))
        {
            // The field is expected to be of the exact size as the struct with 0 offset
            JITDUMP("struct has complex layout - bail out.");
            return nullptr;
        }

        const int bufferSize         = TARGET_POINTER_SIZE;
        uint8_t   buffer[bufferSize] = {0};

        if ((totalSize > bufferSize) || !info.compCompHnd->getStaticFieldContent(field, buffer, totalSize))
        {
            return nullptr;
        }

        unsigned structTempNum = lvaGrabTemp(true DEBUGARG("folding static readonly field struct"));
        lvaSetStruct(structTempNum, fieldClsHnd, false);

        GenTree* constValTree = gtNewGenericCon(fieldVarType, buffer);
        assert(constValTree != nullptr);

        GenTree* fieldStoreTree = gtNewStoreLclFldNode(structTempNum, fieldVarType, fldOffset, constValTree);
        impAppendTree(fieldStoreTree, CHECK_SPILL_NONE, impCurStmtDI);

        JITDUMP("Folding 'static readonly %s' field to a STORE_LCL_FLD(CNS) node\n", eeGetClassName(fieldClsHnd));

        return impCreateLocalNode(structTempNum DEBUGARG(0));
    }
    return nullptr;
}

//------------------------------------------------------------------------
// impImportStaticFieldAddress: Generate an address of a static field
//
// Arguments:
//   pResolvedToken - resolved token for the static field to access
//   access         - type of access to the field, distinguishes address vs load/store
//   pFieldInfo     - EE instructions for accessing the field
//   lclTyp         - type of the field
//   pIndirFlags    - in/out parameter for the field indirection flags (e. g. IND_INITCLASS)
//   pIsHoistable   - optional out parameter - whether any type initialization side
//                    effects of the returned tree can be hoisted to occur earlier
//
// Return Value:
//   Tree representing the field's address.
//
// Notes:
//   Ordinary static fields never overlap. RVA statics, however, can overlap (if they're
//   mapped to the same ".data" declaration). That said, such mappings only appear to be
//   possible with ILASM, and in ILASM-produced (ILONLY) images, RVA statics are always
//   read-only (using "stsfld" on them is UB). In mixed-mode assemblies, RVA statics can
//   be mutable, but the only current producer of such images, the C++/CLI compiler, does
//   not appear to support mapping different fields to the same address. So we will say
//   that "mutable overlapping RVA statics" are UB as well.
//
GenTree* Compiler::impImportStaticFieldAddress(CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                               CORINFO_ACCESS_FLAGS    access,
                                               CORINFO_FIELD_INFO*     pFieldInfo,
                                               var_types               lclTyp,
                                               GenTreeFlags*           pIndirFlags,
                                               bool*                   pIsHoistable)
{
    // For statics that are not "boxed", the initial address tree will contain the field sequence.
    // For those that are, we will attach it later, when adding the indirection for the box, since
    // that tree will represent the true address.
    bool isBoxedStatic  = (pFieldInfo->fieldFlags & CORINFO_FLG_FIELD_STATIC_IN_HEAP) != 0;
    bool isSharedStatic = (pFieldInfo->fieldAccessor == CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER) ||
                          (pFieldInfo->fieldAccessor == CORINFO_FIELD_STATIC_READYTORUN_HELPER);
    FieldSeq::FieldKind fieldKind =
        isSharedStatic ? FieldSeq::FieldKind::SharedStatic : FieldSeq::FieldKind::SimpleStatic;

    bool hasConstAddr = (pFieldInfo->fieldAccessor == CORINFO_FIELD_STATIC_ADDRESS) ||
                        (pFieldInfo->fieldAccessor == CORINFO_FIELD_STATIC_RVA_ADDRESS);

    FieldSeq* innerFldSeq;
    FieldSeq* outerFldSeq;
    if (isBoxedStatic)
    {
        innerFldSeq = nullptr;
        outerFldSeq = GetFieldSeqStore()->Create(pResolvedToken->hField, TARGET_POINTER_SIZE, fieldKind);
    }
    else
    {
        ssize_t offset;
        if (hasConstAddr)
        {
            // Change SimpleStatic to SimpleStaticKnownAddress
            assert(fieldKind == FieldSeq::FieldKind::SimpleStatic);
            fieldKind = FieldSeq::FieldKind::SimpleStaticKnownAddress;

            assert(pFieldInfo->fieldLookup.accessType == IAT_VALUE);
            offset = reinterpret_cast<ssize_t>(pFieldInfo->fieldLookup.addr);
        }
        else
        {
            offset = pFieldInfo->offset;
        }

        innerFldSeq = GetFieldSeqStore()->Create(pResolvedToken->hField, offset, fieldKind);
        outerFldSeq = nullptr;
    }

    bool         isHoistable = false;
    unsigned     typeIndex   = 0;
    GenTreeFlags indirFlags  = GTF_EMPTY;
    GenTree*     op1;
    switch (pFieldInfo->fieldAccessor)
    {
        case CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER:
        {
            // We first call a special helper to get the statics base pointer
            op1 = impParentClassTokenToHandle(pResolvedToken);

            // compIsForInlining() is false so we should not get NULL here
            assert(op1 != nullptr);

            var_types type = TYP_BYREF;

            switch (pFieldInfo->helper)
            {
                case CORINFO_HELP_GET_NONGCTHREADSTATIC_BASE:
                case CORINFO_HELP_GET_GCSTATIC_BASE:
                case CORINFO_HELP_GET_NONGCSTATIC_BASE:
                case CORINFO_HELP_GET_GCTHREADSTATIC_BASE:
                    break;
                default:
                    assert(!"unknown generic statics helper");
                    break;
            }

            isHoistable = !s_helperCallProperties.MayRunCctor(pFieldInfo->helper) ||
                          (info.compCompHnd->getClassAttribs(pResolvedToken->hClass) & CORINFO_FLG_BEFOREFIELDINIT);
            op1 = gtNewHelperCallNode(pFieldInfo->helper, type, op1);
            if (IsStaticHelperEligibleForExpansion(op1))
            {
                // Mark the helper call with the initClsHnd so that rewriting it for expansion can reliably fail
                op1->AsCall()->gtInitClsHnd = pResolvedToken->hClass;
            }
            op1 = gtNewOperNode(GT_ADD, type, op1, gtNewIconNode(pFieldInfo->offset, innerFldSeq));
        }
        break;

        case CORINFO_FIELD_STATIC_TLS_MANAGED:

#ifdef FEATURE_READYTORUN
            if (!IsAot())
#endif // FEATURE_READYTORUN
            {
                if ((pFieldInfo->helper == CORINFO_HELP_GETDYNAMIC_NONGCTHREADSTATIC_BASE_NOCTOR_OPTIMIZED) ||
                    (pFieldInfo->helper == CORINFO_HELP_GETDYNAMIC_NONGCTHREADSTATIC_BASE_NOCTOR_OPTIMIZED2) ||
                    (pFieldInfo->helper == CORINFO_HELP_GETDYNAMIC_NONGCTHREADSTATIC_BASE_NOCTOR_OPTIMIZED2_NOJITOPT))
                {
                    typeIndex = info.compCompHnd->getThreadLocalFieldInfo(pResolvedToken->hField, false);
                }
                else
                {
                    assert(pFieldInfo->helper == CORINFO_HELP_GETDYNAMIC_GCTHREADSTATIC_BASE_NOCTOR_OPTIMIZED);
                    typeIndex = info.compCompHnd->getThreadLocalFieldInfo(pResolvedToken->hField, true);
                }
            }

            FALLTHROUGH;
        case CORINFO_FIELD_STATIC_SHARED_STATIC_HELPER:
        {
#ifdef FEATURE_READYTORUN
            if (IsAot())
            {
                GenTreeFlags callFlags = GTF_EMPTY;

                if (!s_helperCallProperties.MayRunCctor(pFieldInfo->helper) ||
                    (info.compCompHnd->getClassAttribs(pResolvedToken->hClass) & CORINFO_FLG_BEFOREFIELDINIT))
                {
                    isHoistable = true;
                    callFlags |= GTF_CALL_HOISTABLE;
                }

                if (pFieldInfo->fieldAccessor == CORINFO_FIELD_STATIC_TLS_MANAGED)
                {
                    assert(pFieldInfo->helper == CORINFO_HELP_READYTORUN_THREADSTATIC_BASE);
                    op1 = gtNewHelperCallNode(CORINFO_HELP_READYTORUN_THREADSTATIC_BASE_NOCTOR, TYP_BYREF);

                    op1->AsCall()->gtInitClsHnd = pResolvedToken->hClass;
                    op1->AsCall()->setEntryPoint(pFieldInfo->fieldLookup);
                    op1->gtFlags |= callFlags;

                    op1 = gtNewOperNode(GT_ADD, op1->TypeGet(), op1, gtNewIconNode(pFieldInfo->offset, innerFldSeq));

                    m_preferredInitCctor = CORINFO_HELP_READYTORUN_GCSTATIC_BASE;
                    break;
                }

                op1 = gtNewHelperCallNode(pFieldInfo->helper, TYP_BYREF);
                if (pResolvedToken->hClass == info.compClassHnd && m_preferredInitCctor == CORINFO_HELP_UNDEF &&
                    (pFieldInfo->helper == CORINFO_HELP_READYTORUN_GCSTATIC_BASE ||
                     pFieldInfo->helper == CORINFO_HELP_READYTORUN_NONGCSTATIC_BASE))
                {
                    m_preferredInitCctor = pFieldInfo->helper;
                }

                if (IsStaticHelperEligibleForExpansion(op1))
                {
                    // Keep class handle attached to the helper call since it's difficult to restore it.
                    op1->AsCall()->gtInitClsHnd = pResolvedToken->hClass;
                }

                op1->gtFlags |= callFlags;

                op1->AsCall()->setEntryPoint(pFieldInfo->fieldLookup);
            }
            else
#endif
            {
                op1         = fgGetStaticsCCtorHelper(pResolvedToken->hClass, pFieldInfo->helper, typeIndex);
                isHoistable = isHoistable || (op1->gtFlags & GTF_CALL_HOISTABLE);
            }

            op1 = gtNewOperNode(GT_ADD, op1->TypeGet(), op1, gtNewIconNode(pFieldInfo->offset, innerFldSeq));
            break;
        }

        case CORINFO_FIELD_STATIC_RELOCATABLE:
        {
#ifdef FEATURE_READYTORUN
            assert(fieldKind == FieldSeq::FieldKind::SimpleStatic);
            assert(innerFldSeq != nullptr);

            size_t fldAddr = (size_t)pFieldInfo->fieldLookup.addr;
            if (pFieldInfo->fieldLookup.accessType == IAT_VALUE)
            {
                op1 = gtNewIconHandleNode(fldAddr, GTF_ICON_STATIC_HDL);
            }
            else
            {
                assert(pFieldInfo->fieldLookup.accessType == IAT_PVALUE);
                op1 = gtNewIndOfIconHandleNode(TYP_I_IMPL, fldAddr, GTF_ICON_STATIC_ADDR_PTR);
            }
            GenTree* offset = gtNewIconNode(pFieldInfo->offset, innerFldSeq);
            isHoistable     = true;
            op1             = gtNewOperNode(GT_ADD, TYP_I_IMPL, op1, offset);
#else
            unreached();
#endif // FEATURE_READYTORUN
        }
        break;

        case CORINFO_FIELD_STATIC_READYTORUN_HELPER:
        {
#ifdef FEATURE_READYTORUN
            assert(IsAot());
            assert(!compIsForInlining());
            CORINFO_LOOKUP_KIND kind;
            info.compCompHnd->getLocationOfThisType(info.compMethodHnd, &kind);
            assert(kind.needsRuntimeLookup);

            GenTree* ctxTree = getRuntimeContextTree(kind.runtimeLookupKind);

            CorInfoHelpFunc helper    = CORINFO_HELP_READYTORUN_GENERIC_STATIC_BASE;
            GenTreeFlags    callFlags = GTF_EMPTY;

            if (!s_helperCallProperties.MayRunCctor(helper) ||
                (info.compCompHnd->getClassAttribs(pResolvedToken->hClass) & CORINFO_FLG_BEFOREFIELDINIT))
            {
                isHoistable = true;
                callFlags |= GTF_CALL_HOISTABLE;
            }
            var_types type = TYP_BYREF;
            op1            = gtNewHelperCallNode(helper, type, ctxTree);
            op1->gtFlags |= callFlags;

            op1->AsCall()->setEntryPoint(pFieldInfo->fieldLookup);
            op1 = gtNewOperNode(GT_ADD, type, op1, gtNewIconNode(pFieldInfo->offset, innerFldSeq));
#else
            unreached();
#endif // FEATURE_READYTORUN
        }
        break;

        default:
        {
            bool isStaticReadOnlyInitedRef = false;

#ifdef TARGET_64BIT
            // TODO-CQ: enable this optimization for 32 bit targets.
            if (!isBoxedStatic && (lclTyp == TYP_REF) && ((access & CORINFO_ACCESS_GET) != 0) &&
                ((*pIndirFlags & GTF_IND_VOLATILE) == 0))
            {
                bool isSpeculative = true;
                if ((info.compCompHnd->getStaticFieldCurrentClass(pResolvedToken->hField, &isSpeculative) !=
                     NO_CLASS_HANDLE))
                {
                    isStaticReadOnlyInitedRef = !isSpeculative;
                }
            }
#endif // TARGET_64BIT

            assert(pFieldInfo->fieldLookup.accessType == IAT_VALUE);
            size_t       fldAddr = reinterpret_cast<size_t>(pFieldInfo->fieldLookup.addr);
            GenTreeFlags handleKind;
            if (isBoxedStatic)
            {
                handleKind = GTF_ICON_STATIC_BOX_PTR;
            }
            else if (isStaticReadOnlyInitedRef)
            {
                handleKind = GTF_ICON_CONST_PTR;
            }
            else
            {
                handleKind = GTF_ICON_STATIC_HDL;
            }
            isHoistable = true;
            op1         = gtNewIconHandleNode(fldAddr, handleKind, innerFldSeq);
            INDEBUG(op1->AsIntCon()->gtTargetHandle = reinterpret_cast<size_t>(pResolvedToken->hField));

            if (pFieldInfo->fieldFlags & CORINFO_FLG_FIELD_INITCLASS)
            {
                indirFlags |= GTF_IND_INITCLASS;
            }
            if (isStaticReadOnlyInitedRef)
            {
                indirFlags |= (GTF_IND_INVARIANT | GTF_IND_NONNULL);
            }
            break;
        }
    }

    if (isBoxedStatic)
    {
        op1 = gtNewIndir(TYP_REF, op1, GTF_IND_NONFAULTING | GTF_IND_INVARIANT | GTF_IND_NONNULL | indirFlags);
        op1 = gtNewOperNode(GT_ADD, TYP_BYREF, op1, gtNewIconNode(TARGET_POINTER_SIZE, outerFldSeq));

        indirFlags &= ~GTF_IND_INITCLASS;
    }

    *pIndirFlags |= indirFlags;

    if (pIsHoistable != nullptr)
    {
        *pIsHoistable = isHoistable;
    }

    return op1;
}

//------------------------------------------------------------------------
// impAnnotateFieldIndir: Set some flags on a field indirection.
//
// Arguments:
//    indir - The field indirection node
//
// Notes:
//    Exists to preserve previous behavior. New code should not call this.
//
void Compiler::impAnnotateFieldIndir(GenTreeIndir* indir)
{
    if (indir->Addr()->OperIs(GT_FIELD_ADDR))
    {
        GenTreeFieldAddr* addr = indir->Addr()->AsFieldAddr();

        if (addr->IsInstance() && addr->GetFldObj()->OperIs(GT_LCL_ADDR))
        {
            indir->gtFlags &= ~GTF_GLOB_REF;
        }
        else
        {
            assert((indir->gtFlags & GTF_GLOB_REF) != 0);
        }

        addr->gtFlags |= GTF_FLD_DEREFERENCED;
    }
}

// In general try to call this before most of the verification work.  Most people expect the access
// exceptions before the verification exceptions.  If you do this after, that usually doesn't happen.  Turns
// out if you can't access something we also think that you're unverifiable for other reasons.
void Compiler::impHandleAccessAllowed(CorInfoIsAccessAllowedResult result, CORINFO_HELPER_DESC* helperCall)
{
    if (result != CORINFO_ACCESS_ALLOWED)
    {
        impHandleAccessAllowedInternal(result, helperCall);
    }
}

void Compiler::impHandleAccessAllowedInternal(CorInfoIsAccessAllowedResult result, CORINFO_HELPER_DESC* helperCall)
{
    switch (result)
    {
        case CORINFO_ACCESS_ALLOWED:
            break;
        case CORINFO_ACCESS_ILLEGAL:
            impInsertHelperCall(helperCall);
            break;
    }
}

void Compiler::impInsertHelperCall(CORINFO_HELPER_DESC* helperInfo)
{
    assert(helperInfo->helperNum != CORINFO_HELP_UNDEF);

    /* TODO-Review:
     * Mark as CSE'able, and hoistable.  Consider marking hoistable unless you're in the inlinee.
     * Also, consider sticking this in the first basic block.
     */
    GenTreeCall* callout = gtNewHelperCallNode(helperInfo->helperNum, TYP_VOID);
    // Add the arguments
    for (unsigned i = helperInfo->numArgs; i > 0; --i)
    {
        const CORINFO_HELPER_ARG& helperArg  = helperInfo->args[i - 1];
        GenTree*                  currentArg = nullptr;
        switch (helperArg.argType)
        {
            case CORINFO_HELPER_ARG_TYPE_Field:
                info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(
                    info.compCompHnd->getFieldClass(helperArg.fieldHandle));
                currentArg = gtNewIconEmbFldHndNode(helperArg.fieldHandle);
                break;
            case CORINFO_HELPER_ARG_TYPE_Method:
                info.compCompHnd->methodMustBeLoadedBeforeCodeIsRun(helperArg.methodHandle);
                currentArg = gtNewIconEmbMethHndNode(helperArg.methodHandle);
                break;
            case CORINFO_HELPER_ARG_TYPE_Class:
                info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(helperArg.classHandle);
                currentArg = gtNewIconEmbClsHndNode(helperArg.classHandle);
                break;
            case CORINFO_HELPER_ARG_TYPE_Module:
                currentArg = gtNewIconEmbScpHndNode(helperArg.moduleHandle);
                break;
            case CORINFO_HELPER_ARG_TYPE_Const:
                currentArg = gtNewIconNode(helperArg.constant);
                break;
            default:
                NO_WAY("Illegal helper arg type");
        }
        callout->gtArgs.PushFront(this, NewCallArg::Primitive(currentArg));
    }

    impAppendTree(callout, CHECK_SPILL_NONE, impCurStmtDI);
}

/********************************************************************************
 *
 * Returns true if the current opcode and and the opcodes following it correspond
 * to a supported tail call IL pattern.
 *
 */
bool Compiler::impIsTailCallILPattern(
    bool tailPrefixed, OPCODE curOpcode, const BYTE* codeAddrOfNextOpcode, const BYTE* codeEnd, bool isRecursive)
{
    // Bail out if the current opcode is not a call.
    if (!impOpcodeIsCallOpcode(curOpcode))
    {
        return false;
    }

#if !FEATURE_TAILCALL_OPT_SHARED_RETURN
    // If shared ret tail opt is not enabled, we will enable
    // it for recursive methods.
    if (isRecursive)
#endif
    {
        // we can actually handle if the ret is in a fallthrough block, as long as that is the only part of the
        // sequence. Make sure we don't go past the end of the IL however.
        codeEnd = min(codeEnd + 1, info.compCode + info.compILCodeSize);
    }

    // Bail out if there is no next opcode after call
    if (codeAddrOfNextOpcode >= codeEnd)
    {
        return false;
    }

    OPCODE nextOpcode = (OPCODE)getU1LittleEndian(codeAddrOfNextOpcode);

    return (nextOpcode == CEE_RET);
}

/*****************************************************************************
 *
 * Determine whether the call could be converted to an implicit tail call
 *
 */
bool Compiler::impIsImplicitTailCallCandidate(
    OPCODE opcode, const BYTE* codeAddrOfNextOpcode, const BYTE* codeEnd, int prefixFlags, bool isRecursive)
{

#if FEATURE_TAILCALL_OPT
    if (!opts.compTailCallOpt)
    {
        return false;
    }

    if (opts.OptimizationDisabled())
    {
        return false;
    }

    // must not be tail prefixed
    if (prefixFlags & PREFIX_TAILCALL_EXPLICIT)
    {
        return false;
    }

#if !FEATURE_TAILCALL_OPT_SHARED_RETURN
    // the block containing call is marked as BBJ_RETURN
    // We allow shared ret tail call optimization on recursive calls even under
    // !FEATURE_TAILCALL_OPT_SHARED_RETURN.
    if (!isRecursive && !compCurBB->KindIs(BBJ_RETURN))
        return false;
#endif // !FEATURE_TAILCALL_OPT_SHARED_RETURN

    // must be call+ret or call+pop+ret
    if (!impIsTailCallILPattern(false, opcode, codeAddrOfNextOpcode, codeEnd, isRecursive))
    {
        return false;
    }

    return true;
#else
    return false;
#endif // FEATURE_TAILCALL_OPT
}

/*****************************************************************************
   For struct return values, re-type the operand in the case where the ABI
   does not use a struct return buffer
 */

//------------------------------------------------------------------------
// impFixupStructReturnType: Adjust a struct value being returned.
//
// In the multi-reg case, we we force IR to be one of the following:
// GT_RETURN(LCL_VAR) or GT_RETURN(CALL). If op is anything other than
// a lclvar or call, it is assigned to a temp, which is then returned.
// In the non-multireg case, the two special helpers with "fake" return
// buffers are handled ("GETFIELDSTRUCT" and "UNBOX_NULLABLE").
//
// Arguments:
//    op - the return value
//
// Return Value:
//    The (possibly modified) value to return.
//
GenTree* Compiler::impFixupStructReturnType(GenTree* op)
{
    assert(varTypeIsStruct(info.compRetType));
    assert(info.compRetBuffArg == BAD_VAR_NUM);

    JITDUMP("\nimpFixupStructReturnType: retyping\n");
    DISPTREE(op);

    if (op->IsCall() && op->AsCall()->ShouldHaveRetBufArg())
    {
        // This must be one of those 'special' helpers that don't really have a return buffer, but instead
        // use it as a way to keep the trees cleaner with fewer address-taken temps. Well now we have to
        // materialize the return buffer as an address-taken temp. Then we can return the temp.
        //
        unsigned tmpNum = lvaGrabTemp(true DEBUGARG("pseudo return buffer"));

        // No need to spill anything as we're about to return.
        impStoreToTemp(tmpNum, op, CHECK_SPILL_NONE);

        op = gtNewLclvNode(tmpNum, info.compRetType);
        JITDUMP("\nimpFixupStructReturnType: created a pseudo-return buffer for a special helper\n");
        DISPTREE(op);

        return op;
    }

    if (compMethodReturnsMultiRegRetType() || op->IsMultiRegNode())
    {
        // We can use any local with multiple registers (it will be forced to memory on mismatch),
        // except for implicit byrefs (they may turn into indirections).
        if (op->OperIs(GT_LCL_VAR) && !lvaIsImplicitByRefLocal(op->AsLclVar()->GetLclNum()))
        {
            // Note that this is a multi-reg return.
            unsigned lclNum                  = op->AsLclVarCommon()->GetLclNum();
            lvaTable[lclNum].lvIsMultiRegRet = true;

            // TODO-1stClassStructs: Handle constant propagation and CSE-ing of multireg returns.
            op->gtFlags |= GTF_DONT_CSE;

            return op;
        }

        // In contrast, we can only use multi-reg calls directly if they have the exact same ABI.
        // Calling convention equality is a conservative approximation for that check.
        if (op->IsCall() &&
            (op->AsCall()->GetUnmanagedCallConv() == info.compCallConv)
#if defined(TARGET_ARMARCH) || defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
            // TODO-Review: this seems unnecessary. Return ABI doesn't change under varargs.
            && !op->AsCall()->IsVarargs()
#endif // defined(TARGET_ARMARCH) || defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
        )
        {
            return op;
        }

        if (op->IsCall())
        {
            // We cannot tail call because control needs to return to fixup the calling convention
            // for result return.
            op->AsCall()->gtCallMoreFlags &= ~GTF_CALL_M_TAILCALL;
            op->AsCall()->gtCallMoreFlags &= ~GTF_CALL_M_EXPLICIT_TAILCALL;
        }

        // The backend does not support other struct-producing nodes (e. g. OBJs) as sources of multi-reg returns.
        // It also does not support assembling a multi-reg node into one register (for RETURN nodes at least).
        return impStoreMultiRegValueToVar(op, info.compMethodInfo->args.retTypeClass DEBUGARG(info.compCallConv));
    }

    // Not a multi-reg return or value, we can simply use it directly.
    return op;
}

//------------------------------------------------------------------------
// impImportLeave: canonicalize flow when leaving a protected region
//
// Arguments:
//   block - block with BBJ_LEAVE jump kind to canonicalize
//
// Notes:
//
//   CEE_LEAVE may be jumping out of a protected block, viz, a catch or a
//   finally-protected try. We find the finally blocks protecting the current
//   offset (in order) by walking over the complete exception table and
//   finding enclosing clauses. This assumes that the table is sorted.
//   This will create a series of BBJ_CALLFINALLY/BBJ_CALLFINALLYRET ->
//   BBJ_CALLFINALLY/BBJ_CALLFINALLYRET ... -> BBJ_ALWAYS.
//
//   If we are leaving a catch handler, we need to attach the
//   ENDCATCHes to the correct BBJ_CALLFINALLY blocks.
//
//   After this function, the BBJ_LEAVE block has been converted to a different type.
//

#if defined(FEATURE_EH_WINDOWS_X86)

void Compiler::impImportLeaveEHRegions(BasicBlock* block)
{
#ifdef DEBUG
    if (verbose)
    {
        printf("\nBefore import CEE_LEAVE:\n");
        fgDispBasicBlocks();
        fgDispHandlerTab();
    }
#endif // DEBUG

    unsigned const    blkAddr     = block->bbCodeOffs;
    BasicBlock* const leaveTarget = block->GetTarget();
    unsigned const    jmpAddr     = leaveTarget->bbCodeOffs;

    // LEAVE clears the stack, spill side effects, and set stack to 0

    impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("impImportLeave"));
    stackState.esStackDepth = 0;

    assert(block->KindIs(BBJ_LEAVE));
    assert(fgBBs == (BasicBlock**)0xCDCD || fgLookupBB(jmpAddr) != NULL); // should be a BB boundary

    BasicBlock* step         = DUMMY_INIT(NULL);
    unsigned    encFinallies = 0; // Number of enclosing finallies.
    GenTree*    endCatches   = NULL;
    Statement*  endLFinStmt  = NULL; // The statement tree to indicate the end of locally-invoked finally.

    unsigned  XTnum;
    EHblkDsc* HBtab;

    for (XTnum = 0, HBtab = compHndBBtab; XTnum < compHndBBtabCount; XTnum++, HBtab++)
    {
        // Grab the handler offsets

        IL_OFFSET tryBeg = HBtab->ebdTryBegOffs();
        IL_OFFSET tryEnd = HBtab->ebdTryEndOffs();
        IL_OFFSET hndBeg = HBtab->ebdHndBegOffs();
        IL_OFFSET hndEnd = HBtab->ebdHndEndOffs();

        // Is this a catch-handler we are CEE_LEAVE'ing out of? If so, we need to call CORINFO_HELP_ENDCATCH.

        if (jitIsBetween(blkAddr, hndBeg, hndEnd) && !jitIsBetween(jmpAddr, hndBeg, hndEnd))
        {
            // Can't CEE_LEAVE out of a finally/fault handler
            if (HBtab->HasFinallyOrFaultHandler())
            {
                BADCODE("leave out of fault/finally block");
            }

            // Create the call to CORINFO_HELP_ENDCATCH
            GenTree* endCatch = gtNewHelperCallNode(CORINFO_HELP_ENDCATCH, TYP_VOID);

            // Make a list of all the currently pending endCatches
            if (endCatches)
            {
                endCatches = gtNewOperNode(GT_COMMA, TYP_VOID, endCatches, endCatch);
            }
            else
            {
                endCatches = endCatch;
            }

#ifdef DEBUG
            if (verbose)
            {
                printf("impImportLeave - " FMT_BB " jumping out of catch handler EH#%u, adding call to "
                       "CORINFO_HELP_ENDCATCH\n",
                       block->bbNum, XTnum);
            }
#endif
        }
        else if (HBtab->HasFinallyHandler() && jitIsBetween(blkAddr, tryBeg, tryEnd) &&
                 !jitIsBetween(jmpAddr, tryBeg, tryEnd))
        {
            // This is a finally-protected try we are jumping out of.
            //
            // If there are any pending endCatches, and we have already jumped out of a finally-protected try,
            // then the endCatches have to be put in a block in an outer try for async exceptions to work correctly.
            // Else, just append to the original block.

            BasicBlock* callBlock;

            // If we have finallies, we better have an endLFin tree, and vice-versa.
            assert(!encFinallies == !endLFinStmt);

            if (encFinallies == 0)
            {
                assert(step == DUMMY_INIT(NULL));
                callBlock = block;

                // callBlock calls the finally handler
                assert(callBlock->HasInitializedTarget());
                fgRedirectEdge(callBlock->TargetEdgeRef(), HBtab->ebdHndBeg);
                callBlock->SetKind(BBJ_CALLFINALLY);

                if (endCatches)
                {
                    impAppendTree(endCatches, CHECK_SPILL_NONE, impCurStmtDI);
                }

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a finally-protected try, convert block to BBJ_CALLFINALLY "
                           "block %s\n",
                           callBlock->dspToString());
                }
#endif
            }
            else
            {
                assert(step != DUMMY_INIT(NULL));

                // Calling the finally block.

                // callBlock calls the finally handler
                callBlock = fgNewBBinRegion(BBJ_CALLFINALLY, XTnum + 1, 0, step);

                {
                    FlowEdge* const newEdge = fgAddRefPred(HBtab->ebdHndBeg, callBlock);
                    callBlock->SetTargetEdge(newEdge);
                }

                // step's jump target shouldn't be set yet
                assert(!step->HasInitializedTarget());

                {
                    // the previous call to a finally returns to this call (to the next finally in the chain)
                    FlowEdge* const newEdge = fgAddRefPred(callBlock, step);
                    step->SetTargetEdge(newEdge);
                }

                // The new block will inherit this block's weight.
                callBlock->inheritWeight(block);

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a finally-protected try, new BBJ_CALLFINALLY block %s\n",
                           callBlock->dspToString());
                }
#endif

                Statement* lastStmt;

                if (endCatches)
                {
                    lastStmt = gtNewStmt(endCatches);
                    endLFinStmt->SetNextStmt(lastStmt);
                    lastStmt->SetPrevStmt(endLFinStmt);
                }
                else
                {
                    lastStmt = endLFinStmt;
                }

                // note that this sets BBF_IMPORTED on the block
                impEndTreeList(callBlock, endLFinStmt, lastStmt);
            }

            // callBlock should be set up at this point
            assert(callBlock->TargetIs(HBtab->ebdHndBeg));

            // Note: we don't know the jump target yet
            step = fgNewBBafter(BBJ_CALLFINALLYRET, callBlock, true);
            // The new block will inherit this block's weight.
            step->inheritWeight(block);
            step->SetFlags(BBF_IMPORTED);

#ifdef DEBUG
            if (verbose)
            {
                printf("impImportLeave - jumping out of a finally-protected try, created step (BBJ_CALLFINALLYRET) "
                       "block %s\n",
                       step->dspToString());
            }
#endif

            // We now record the EH region ID on GT_END_LFIN instead of the finally nesting depth,
            // as the later can change as we optimize the code.
            //
            unsigned const ehID = compHndBBtab[XTnum].ebdID;
            assert(ehID <= impInlineRoot()->compEHID);

            GenTree* const endLFin = new (this, GT_END_LFIN) GenTreeVal(GT_END_LFIN, TYP_VOID, ehID);
            endLFinStmt            = gtNewStmt(endLFin);
            endCatches             = NULL;

            encFinallies++;
        }
    }

    // Append any remaining endCatches, if any.

    assert(!encFinallies == !endLFinStmt);

    if (encFinallies == 0)
    {
        assert(step == DUMMY_INIT(NULL));
        block->SetKind(BBJ_ALWAYS); // convert the BBJ_LEAVE to a BBJ_ALWAYS

        if (endCatches)
        {
            impAppendTree(endCatches, CHECK_SPILL_NONE, impCurStmtDI);
        }

#ifdef DEBUG
        if (verbose)
        {
            printf("impImportLeave - no enclosing finally-protected try blocks; convert CEE_LEAVE block to BBJ_ALWAYS "
                   "block %s\n",
                   block->dspToString());
        }
#endif
    }
    else
    {
        // If leaveTarget is the start of another try block, we want to make sure that
        // we do not insert finalStep into that try block. Hence, we find the enclosing
        // try block.
        unsigned tryIndex = bbFindInnermostCommonTryRegion(step, leaveTarget);

        // Insert a new BB either in the try region indicated by tryIndex or
        // the handler region indicated by leaveTarget->bbHndIndex,
        // depending on which is the inner region.
        BasicBlock* finalStep = fgNewBBinRegion(BBJ_ALWAYS, tryIndex, leaveTarget->bbHndIndex, step);
        finalStep->SetFlags(BBF_KEEP_BBJ_ALWAYS);

        // step's jump target shouldn't be set yet
        assert(!step->HasInitializedTarget());

        {
            FlowEdge* const newEdge = fgAddRefPred(finalStep, step);
            step->SetTargetEdge(newEdge);
        }

        // The new block will inherit this block's weight.
        finalStep->inheritWeight(block);

#ifdef DEBUG
        if (verbose)
        {
            printf("impImportLeave - finalStep block required (encFinallies(%d) > 0), new block %s\n", encFinallies,
                   finalStep->dspToString());
        }
#endif

        Statement* lastStmt;

        if (endCatches)
        {
            lastStmt = gtNewStmt(endCatches);
            endLFinStmt->SetNextStmt(lastStmt);
            lastStmt->SetPrevStmt(endLFinStmt);
        }
        else
        {
            lastStmt = endLFinStmt;
        }

        impEndTreeList(finalStep, endLFinStmt, lastStmt);

        // this is the ultimate destination of the LEAVE
        {
            FlowEdge* const newEdge = fgAddRefPred(leaveTarget, finalStep);
            finalStep->SetTargetEdge(newEdge);
        }

        // Queue up the jump target for importing

        impImportBlockPending(leaveTarget);
    }

#ifdef DEBUG
    fgVerifyHandlerTab();

    if (verbose)
    {
        printf("\nAfter import CEE_LEAVE:\n");
        fgDispBasicBlocks();
        fgDispHandlerTab();
    }
#endif // DEBUG
}

#endif // FEATURE_EH_WINDOWS_X86

void Compiler::impImportLeave(BasicBlock* block)
{
#if defined(FEATURE_EH_WINDOWS_X86)
    if (!UsesFunclets())
    {
        return impImportLeaveEHRegions(block);
    }
#endif

#ifdef DEBUG
    if (verbose)
    {
        printf("\nBefore import CEE_LEAVE in " FMT_BB " (targeting " FMT_BB "):\n", block->bbNum,
               block->GetTarget()->bbNum);
        fgDispBasicBlocks();
        fgDispHandlerTab();
    }
#endif // DEBUG

    unsigned    blkAddr     = block->bbCodeOffs;
    BasicBlock* leaveTarget = block->GetTarget();
    unsigned    jmpAddr     = leaveTarget->bbCodeOffs;

    // LEAVE clears the stack, spill side effects, and set stack to 0

    impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("impImportLeave"));
    stackState.esStackDepth = 0;

    assert(block->KindIs(BBJ_LEAVE));
    assert(fgBBs == (BasicBlock**)0xCDCD || fgLookupBB(jmpAddr) != nullptr); // should be a BB boundary

    BasicBlock* step = nullptr;

    enum StepType
    {
        // No step type; step == NULL.
        ST_None,

        // The step block is the BBJ_CALLFINALLYRET block of a BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pair.
        // That is, is step->GetFinallyContinuation() is where a finally will return to.
        ST_FinallyReturn,

        // The step block is a catch return.
        ST_Catch,

        // The step block is in a "try", created as the target for a finally return or the target for a catch return.
        ST_Try
    };
    StepType stepType = ST_None;

    unsigned  XTnum;
    EHblkDsc* HBtab;

    for (XTnum = 0, HBtab = compHndBBtab; XTnum < compHndBBtabCount; XTnum++, HBtab++)
    {
        // Grab the handler offsets

        IL_OFFSET tryBeg = HBtab->ebdTryBegOffs();
        IL_OFFSET tryEnd = HBtab->ebdTryEndOffs();
        IL_OFFSET hndBeg = HBtab->ebdHndBegOffs();
        IL_OFFSET hndEnd = HBtab->ebdHndEndOffs();

        // Is this a catch-handler we are CEE_LEAVE'ing out of?

        if (jitIsBetween(blkAddr, hndBeg, hndEnd) && !jitIsBetween(jmpAddr, hndBeg, hndEnd))
        {
            // Can't CEE_LEAVE out of a finally/fault handler
            if (HBtab->HasFinallyOrFaultHandler())
            {
                BADCODE("leave out of fault/finally block");
            }

            // We are jumping out of a catch.

            if (step == nullptr)
            {
                step = block;
                step->SetKind(BBJ_EHCATCHRET); // convert the BBJ_LEAVE to BBJ_EHCATCHRET
                stepType = ST_Catch;

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a catch (EH#%u), convert block " FMT_BB
                           " to BBJ_EHCATCHRET block\n",
                           XTnum, step->bbNum);
                }
#endif
            }
            else
            {
                // Create a new catch exit block in the catch region for the existing step block to jump to in this
                // scope.
                // Note: we don't know the jump target yet
                BasicBlock* exitBlock = fgNewBBinRegion(BBJ_EHCATCHRET, 0, XTnum + 1, step);

                assert(step->KindIs(BBJ_ALWAYS, BBJ_CALLFINALLYRET, BBJ_EHCATCHRET));
                assert((step == block) || !step->HasInitializedTarget());
                if (step == block)
                {
                    fgRedirectEdge(step->TargetEdgeRef(), exitBlock);
                }
                else
                {
                    FlowEdge* const newEdge = fgAddRefPred(exitBlock, step);
                    step->SetTargetEdge(newEdge); // the previous step (maybe a call to a nested finally, or a nested
                                                  // catch
                                                  // exit) returns to this block
                }

                // The new block will inherit this block's weight.
                exitBlock->inheritWeight(block);
                exitBlock->SetFlags(BBF_IMPORTED);

                // This exit block is the new step.
                step     = exitBlock;
                stepType = ST_Catch;

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a catch (EH#%u), new BBJ_EHCATCHRET block " FMT_BB "\n",
                           XTnum, exitBlock->bbNum);
                }
#endif
            }
        }
        else if (HBtab->HasFinallyHandler() && jitIsBetween(blkAddr, tryBeg, tryEnd) &&
                 !jitIsBetween(jmpAddr, tryBeg, tryEnd))
        {
            // We are jumping out of a finally-protected try.

            BasicBlock* callBlock;

            if (step == nullptr && UsesCallFinallyThunks())
            {
                // Put the call to the finally in the enclosing region.
                unsigned callFinallyTryIndex =
                    (HBtab->ebdEnclosingTryIndex == EHblkDsc::NO_ENCLOSING_INDEX) ? 0 : HBtab->ebdEnclosingTryIndex + 1;
                unsigned callFinallyHndIndex =
                    (HBtab->ebdEnclosingHndIndex == EHblkDsc::NO_ENCLOSING_INDEX) ? 0 : HBtab->ebdEnclosingHndIndex + 1;
                callBlock = fgNewBBinRegion(BBJ_CALLFINALLY, callFinallyTryIndex, callFinallyHndIndex, block);

                // Convert the BBJ_LEAVE to BBJ_ALWAYS, jumping to the new BBJ_CALLFINALLY. This is because
                // the new BBJ_CALLFINALLY is in a different EH region, thus it can't just replace the BBJ_LEAVE,
                // which might be in the middle of the "try". In most cases, the BBJ_ALWAYS will jump to the
                // next block, and flow optimizations will remove it.
                fgRedirectEdge(block->TargetEdgeRef(), callBlock);
                block->SetKind(BBJ_ALWAYS);

                // The new block will inherit this block's weight.
                callBlock->inheritWeight(block);
                callBlock->SetFlags(BBF_IMPORTED);

                // callBlock calls the finally handler
                FlowEdge* const newEdge = fgAddRefPred(HBtab->ebdHndBeg, callBlock);
                callBlock->SetKindAndTargetEdge(BBJ_CALLFINALLY, newEdge);

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a finally-protected try (EH#%u), convert block " FMT_BB
                           " to BBJ_ALWAYS, add BBJ_CALLFINALLY block " FMT_BB "\n",
                           XTnum, block->bbNum, callBlock->bbNum);
                }
#endif
            }
            else if (step == nullptr) // && !UsesCallFinallyThunks()
            {
                callBlock = block;

                // callBlock calls the finally handler
                assert(callBlock->HasInitializedTarget());
                fgRedirectEdge(callBlock->TargetEdgeRef(), HBtab->ebdHndBeg);
                callBlock->SetKind(BBJ_CALLFINALLY);

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a finally-protected try (EH#%u), convert block " FMT_BB
                           " to BBJ_CALLFINALLY block\n",
                           XTnum, callBlock->bbNum);
                }
#endif
            }
            else
            {
                // Calling the finally block. We already have a step block that is either the call-to-finally from a
                // more nested try/finally (thus we are jumping out of multiple nested 'try' blocks, each protected by
                // a 'finally'), or the step block is the return from a catch.
                //
                // Due to ThreadAbortException, we can't have the catch return target the call-to-finally block
                // directly. Note that if a 'catch' ends without resetting the ThreadAbortException, the VM will
                // automatically re-raise the exception, using the return address of the catch (that is, the target
                // block of the BBJ_EHCATCHRET) as the re-raise address. If this address is in a finally, the VM will
                // refuse to do the re-raise, and the ThreadAbortException will get eaten (and lost). On AMD64/ARM64,
                // we put the call-to-finally thunk in a special "cloned finally" EH region that does look like a
                // finally clause to the VM. Thus, on these platforms, we can't have BBJ_EHCATCHRET target a
                // BBJ_CALLFINALLY directly. (Note that on ARM32, we don't mark the thunk specially -- it lives directly
                // within the 'try' region protected by the finally, since we generate code in such a way that execution
                // never returns to the call-to-finally call, and the finally-protected 'try' region doesn't appear on
                // stack walks.)

                assert(step->KindIs(BBJ_ALWAYS, BBJ_CALLFINALLYRET, BBJ_EHCATCHRET));
                assert((step == block) || !step->HasInitializedTarget());

                if (UsesCallFinallyThunks() && step->KindIs(BBJ_EHCATCHRET))
                {
                    // Need to create another step block in the 'try' region that will actually branch to the
                    // call-to-finally thunk.
                    // Note: we don't know the jump target yet
                    BasicBlock* step2 = fgNewBBinRegion(BBJ_ALWAYS, XTnum + 1, 0, step);
                    if (step == block)
                    {
                        fgRedirectEdge(step->TargetEdgeRef(), step2);
                    }
                    else
                    {
                        FlowEdge* const newEdge = fgAddRefPred(step2, step);
                        step->SetTargetEdge(newEdge);
                    }

                    step2->inheritWeight(block);
                    step2->SetFlags(BBF_IMPORTED);

#ifdef DEBUG
                    if (verbose)
                    {
                        printf("impImportLeave - jumping out of a finally-protected try (EH#%u), step block is "
                               "BBJ_EHCATCHRET (" FMT_BB "), new BBJ_ALWAYS step-step block " FMT_BB "\n",
                               XTnum, step->bbNum, step2->bbNum);
                    }
#endif

                    step = step2;
                    assert(stepType == ST_Catch); // Leave it as catch type for now.
                }

                unsigned callFinallyTryIndex;
                unsigned callFinallyHndIndex;

                if (UsesCallFinallyThunks())
                {
                    callFinallyTryIndex = (HBtab->ebdEnclosingTryIndex == EHblkDsc::NO_ENCLOSING_INDEX)
                                              ? 0
                                              : HBtab->ebdEnclosingTryIndex + 1;
                    callFinallyHndIndex = (HBtab->ebdEnclosingHndIndex == EHblkDsc::NO_ENCLOSING_INDEX)
                                              ? 0
                                              : HBtab->ebdEnclosingHndIndex + 1;
                }
                else
                {
                    callFinallyTryIndex = XTnum + 1;
                    callFinallyHndIndex = 0; // don't care
                }

                assert(step->KindIs(BBJ_ALWAYS, BBJ_CALLFINALLYRET, BBJ_EHCATCHRET));
                assert((step == block) || !step->HasInitializedTarget());

                // callBlock will call the finally handler
                callBlock = fgNewBBinRegion(BBJ_CALLFINALLY, callFinallyTryIndex, callFinallyHndIndex, step);
                if (step == block)
                {
                    fgRedirectEdge(step->TargetEdgeRef(), callBlock);
                }
                else
                {
                    FlowEdge* const newEdge = fgAddRefPred(callBlock, step);
                    step->SetTargetEdge(newEdge); // the previous call to a finally returns to this call (to the next
                                                  // finally in the chain)
                }

                // The new block will inherit this block's weight.
                callBlock->inheritWeight(block);
                callBlock->SetFlags(BBF_IMPORTED);

                // callBlock calls the finally handler
                FlowEdge* const newEdge = fgAddRefPred(HBtab->ebdHndBeg, callBlock);
                callBlock->SetKindAndTargetEdge(BBJ_CALLFINALLY, newEdge);

#ifdef DEBUG
                if (verbose)
                {
                    printf("impImportLeave - jumping out of a finally-protected try (EH#%u), new BBJ_CALLFINALLY "
                           "block " FMT_BB "\n",
                           XTnum, callBlock->bbNum);
                }
#endif
            }

            // callBlock should be set up at this point
            assert(callBlock->TargetIs(HBtab->ebdHndBeg));

            // Note: we don't know the jump target yet
            step     = fgNewBBafter(BBJ_CALLFINALLYRET, callBlock, true);
            stepType = ST_FinallyReturn;

            // The new block will inherit this block's weight.
            step->inheritWeight(block);
            step->SetFlags(BBF_IMPORTED);

#ifdef DEBUG
            if (verbose)
            {
                printf("impImportLeave - jumping out of a finally-protected try (EH#%u), created step "
                       "(BBJ_CALLFINALLYRET) "
                       "block " FMT_BB "\n",
                       XTnum, step->bbNum);
            }
#endif
        }
        else if (HBtab->HasCatchHandler() && jitIsBetween(blkAddr, tryBeg, tryEnd) &&
                 !jitIsBetween(jmpAddr, tryBeg, tryEnd))
        {
            // We are jumping out of a catch-protected try.
            //
            // If we are returning from a call to a finally, then we must have a step block within a try
            // that is protected by a catch. This is so when unwinding from that finally (e.g., if code within the
            // finally raises an exception), the VM will find this step block, notice that it is in a protected region,
            // and invoke the appropriate catch.
            //
            // We also need to handle a special case with the handling of ThreadAbortException. If a try/catch
            // catches a ThreadAbortException (which might be because it catches a parent, e.g. System.Exception),
            // and the catch doesn't call System.Threading.Thread::ResetAbort(), then when the catch returns to the VM,
            // the VM will automatically re-raise the ThreadAbortException. When it does this, it uses the target
            // address of the catch return as the new exception address. That is, the re-raised exception appears to
            // occur at the catch return address. If this exception return address skips an enclosing try/catch that
            // catches ThreadAbortException, then the enclosing try/catch will not catch the exception, as it should.
            // For example:
            //
            // try {
            //    try {
            //       // something here raises ThreadAbortException
            //       LEAVE LABEL_1; // no need to stop at LABEL_2
            //    } catch (Exception) {
            //       // This catches ThreadAbortException, but doesn't call System.Threading.Thread::ResetAbort(), so
            //       // ThreadAbortException is re-raised by the VM at the address specified by the LEAVE opcode.
            //       // This is bad, since it means the outer try/catch won't get a chance to catch the re-raised
            //       // ThreadAbortException. So, instead, create step block LABEL_2 and LEAVE to that. We only
            //       // need to do this transformation if the current EH block is a try/catch that catches
            //       // ThreadAbortException (or one of its parents), however we might not be able to find that
            //       // information, so currently we do it for all catch types.
            //       LEAVE LABEL_1; // Convert this to LEAVE LABEL2;
            //    }
            //    LABEL_2: LEAVE LABEL_1; // inserted by this step creation code
            // } catch (ThreadAbortException) {
            // }
            // LABEL_1:
            //
            // Note that this pattern isn't theoretical: it occurs in ASP.NET, in IL code generated by the Roslyn C#
            // compiler.

            if ((stepType == ST_FinallyReturn) || (stepType == ST_Catch))
            {
                assert(step);
                assert((step == block) || !step->HasInitializedTarget());

                if (stepType == ST_FinallyReturn)
                {
                    assert(step->KindIs(BBJ_CALLFINALLYRET));
                }
                else
                {
                    assert(stepType == ST_Catch);
                    assert(step->KindIs(BBJ_EHCATCHRET));
                }

                // Create a new exit block in the try region for the existing step block to jump to in this scope.
                // Note: we don't know the jump target yet
                BasicBlock* catchStep = fgNewBBinRegion(BBJ_ALWAYS, XTnum + 1, 0, step);

                if (step == block)
                {
                    fgRedirectEdge(step->TargetEdgeRef(), catchStep);
                }
                else
                {
                    FlowEdge* const newEdge = fgAddRefPred(catchStep, step);
                    step->SetTargetEdge(newEdge);
                }

                // The new block will inherit this block's weight.
                catchStep->inheritWeight(block);
                catchStep->SetFlags(BBF_IMPORTED);

#ifdef DEBUG
                if (verbose)
                {
                    if (stepType == ST_FinallyReturn)
                    {
                        printf("impImportLeave - return from finally jumping out of a catch-protected try (EH#%u), new "
                               "BBJ_ALWAYS block " FMT_BB "\n",
                               XTnum, catchStep->bbNum);
                    }
                    else
                    {
                        assert(stepType == ST_Catch);
                        printf("impImportLeave - return from catch jumping out of a catch-protected try (EH#%u), new "
                               "BBJ_ALWAYS block " FMT_BB "\n",
                               XTnum, catchStep->bbNum);
                    }
                }
#endif // DEBUG

                // This block is the new step.
                step     = catchStep;
                stepType = ST_Try;
            }
        }
    }

    if (step == nullptr)
    {
        block->SetKind(BBJ_ALWAYS); // convert the BBJ_LEAVE to a BBJ_ALWAYS

#ifdef DEBUG
        if (verbose)
        {
            printf("impImportLeave - no enclosing finally-protected try blocks or catch handlers; convert CEE_LEAVE "
                   "block " FMT_BB " to BBJ_ALWAYS\n",
                   block->bbNum);
        }
#endif
    }
    else
    {
        assert((step == block) || !step->HasInitializedTarget());

        // leaveTarget is the ultimate destination of the LEAVE
        if (step == block)
        {
            fgRedirectEdge(step->TargetEdgeRef(), leaveTarget);
        }
        else
        {
            FlowEdge* const newEdge = fgAddRefPred(leaveTarget, step);
            step->SetTargetEdge(newEdge);
        }

#ifdef DEBUG
        if (verbose)
        {
            printf("impImportLeave - final destination of step blocks set to " FMT_BB "\n", leaveTarget->bbNum);
        }
#endif

        // Queue up the jump target for importing

        impImportBlockPending(leaveTarget);
    }

#ifdef DEBUG
    fgVerifyHandlerTab();

    if (verbose)
    {
        printf("\nAfter import CEE_LEAVE:\n");
        fgDispBasicBlocks();
        fgDispHandlerTab();
    }
#endif // DEBUG
}

/*****************************************************************************/
// This is called when reimporting a leave block. It resets the JumpKind,
// JumpDest, and bbNext to the original values

void Compiler::impResetLeaveBlock(BasicBlock* block, unsigned jmpAddr)
{
    // With EH Funclets, while importing leave opcode we create another block ending with BBJ_ALWAYS (call it B1)
    // and the block containing leave (say B0) is marked as BBJ_CALLFINALLY.   Say for some reason we reimport B0,
    // it is reset (in this routine) by marking as ending with BBJ_LEAVE and further down when B0 is reimported, we
    // create another BBJ_ALWAYS (call it B2). In this process B1 gets orphaned and any blocks to which B1 is the
    // only predecessor are also considered orphans and attempted to be deleted.
    //
    //  try  {
    //     ....
    //     try
    //     {
    //         ....
    //         leave OUTSIDE;  // B0 is the block containing this leave, following this would be B1
    //     } finally { }
    //  } finally { }
    //  OUTSIDE:
    //
    // In the above nested try-finally example, we create a step block (call it Bstep) which in branches to a block
    // where a finally would branch to (and such block is marked as finally target).  Block B1 branches to step block.
    // Because of re-import of B0, Bstep is also orphaned. Since Bstep is a finally target it cannot be removed.  To
    // work around this we will duplicate B0 (call it B0Dup) before resetting. B0Dup is marked as BBJ_CALLFINALLY and
    // only serves to pair up with B1 (BBJ_ALWAYS) that got orphaned. Now during orphan block deletion B0Dup and B1
    // will be treated as pair and handled correctly.
    if (UsesFunclets() && block->KindIs(BBJ_CALLFINALLY))
    {
        BasicBlock* dupBlock = BasicBlock::New(this);
        dupBlock->CopyFlags(block);
        FlowEdge* const newEdge = fgAddRefPred(block->GetTarget(), dupBlock);
        dupBlock->SetKindAndTargetEdge(BBJ_CALLFINALLY, newEdge);
        dupBlock->copyEHRegion(block);
        dupBlock->bbCatchTyp = block->bbCatchTyp;

        // Mark this block as
        //  a) not referenced by any other block to make sure that it gets deleted
        //  b) weight zero
        //  c) prevent from being imported
        //  d) as internal
        dupBlock->bbRefs = 0;
        dupBlock->bbSetRunRarely();
        dupBlock->SetFlags(BBF_IMPORTED | BBF_INTERNAL);

        // Insert the block right after the block which is getting reset so that BBJ_CALLFINALLY and BBJ_ALWAYS
        // will be next to each other.
        fgInsertBBafter(block, dupBlock);

#ifdef DEBUG
        if (verbose)
        {
            printf("New Basic Block " FMT_BB " duplicate of " FMT_BB " created.\n", dupBlock->bbNum, block->bbNum);
        }
#endif
    }

    fgInitBBLookup();

    fgRedirectEdge(block->TargetEdgeRef(), fgLookupBB(jmpAddr));
    block->SetKind(BBJ_LEAVE);

    // We will leave the BBJ_ALWAYS block we introduced. When it's reimported
    // the BBJ_ALWAYS block will be unreachable, and will be removed after. The
    // reason we don't want to remove the block at this point is that if we call
    // fgInitBBLookup() again we will do it wrong as the BBJ_ALWAYS block won't be
    // added and the linked list length will be different than fgBBcount.
    //
    // Because of this incomplete cleanup. profile data may be left inconsistent.
    //
    if (block->hasProfileWeight())
    {
        // We are unlikely to be able to repair the profile.
        // For now we don't even try.
        //
        JITDUMP("\nimpResetLeaveBlock: Profile data could not be locally repaired. Data %s inconsistent.\n",
                fgPgoConsistent ? "is now" : "was already");

        if (fgPgoConsistent)
        {
            Metrics.ProfileInconsistentResetLeave++;
            fgPgoConsistent = false;
        }
    }
}

/*****************************************************************************/
// Get the first non-prefix opcode. Used for verification of valid combinations
// of prefixes and actual opcodes.

OPCODE Compiler::impGetNonPrefixOpcode(const BYTE* codeAddr, const BYTE* codeEndp)
{
    while (codeAddr < codeEndp)
    {
        OPCODE opcode = (OPCODE)getU1LittleEndian(codeAddr);
        codeAddr += sizeof(int8_t);

        if (opcode == CEE_PREFIX1)
        {
            if (codeAddr >= codeEndp)
            {
                break;
            }
            opcode = (OPCODE)(getU1LittleEndian(codeAddr) + 256);
            codeAddr += sizeof(int8_t);
        }

        switch (opcode)
        {
            case CEE_UNALIGNED:
            case CEE_VOLATILE:
            case CEE_TAILCALL:
            case CEE_CONSTRAINED:
            case CEE_READONLY:
                break;
            default:
                return opcode;
        }

        codeAddr += opcodeSizes[opcode];
    }

    return CEE_ILLEGAL;
}

GenTreeFlags Compiler::impPrefixFlagsToIndirFlags(unsigned prefixFlags)
{
    GenTreeFlags indirFlags = GTF_EMPTY;
    if ((prefixFlags & PREFIX_VOLATILE) != 0)
    {
        indirFlags |= GTF_IND_VOLATILE;
    }
    if ((prefixFlags & PREFIX_UNALIGNED) != 0)
    {
        indirFlags |= GTF_IND_UNALIGNED;
    }

    return indirFlags;
}

/*****************************************************************************/
// Checks whether the opcode is a valid opcode for volatile. and unaligned. prefixes

void Compiler::impValidateMemoryAccessOpcode(const BYTE* codeAddr, const BYTE* codeEndp, bool volatilePrefix)
{
    OPCODE opcode = impGetNonPrefixOpcode(codeAddr, codeEndp);

    if (!(
            // Opcode of all ldind and stdind happen to be in continuous, except stind.i.
            ((CEE_LDIND_I1 <= opcode) && (opcode <= CEE_STIND_R8)) || (opcode == CEE_STIND_I) ||
            (opcode == CEE_LDFLD) || (opcode == CEE_STFLD) || (opcode == CEE_LDOBJ) || (opcode == CEE_STOBJ) ||
            (opcode == CEE_INITBLK) || (opcode == CEE_CPBLK) ||
            // volatile. prefix is allowed with the ldsfld and stsfld
            (volatilePrefix && ((opcode == CEE_LDSFLD) || (opcode == CEE_STSFLD)))))
    {
        BADCODE("Invalid opcode for unaligned. or volatile. prefix");
    }
}

/*****************************************************************************
 *  Determine the result type of an arithmetic operation
 *  On 64-bit inserts upcasts when native int is mixed with int32
 *  Also inserts upcasts to double when float and double are mixed.
 */
var_types Compiler::impGetByRefResultType(genTreeOps oper, bool fUnsigned, GenTree** pOp1, GenTree** pOp2)
{
    var_types type = TYP_UNDEF;
    GenTree*  op1  = *pOp1;
    GenTree*  op2  = *pOp2;

    assert(op1 != nullptr);
    assert(op2 != nullptr);

    // Arithmetic operations are generally only allowed with primitive types, but certain operations are allowed
    // with byrefs.
    //
    if ((oper == GT_SUB) && (op1->TypeIs(TYP_BYREF) || op2->TypeIs(TYP_BYREF)))
    {
        if (op1->TypeIs(TYP_BYREF) && op2->TypeIs(TYP_BYREF))
        {
            // byref1-byref2 => gives a native int
            type = TYP_I_IMPL;
        }
        else if (genActualTypeIsIntOrI(op1) && op2->TypeIs(TYP_BYREF))
        {
            // [native] int - byref => gives a native int

            //
            // The reason is that it is possible, in managed C++,
            // to have a tree like this:
            //
            //              -
            //             / \.
            //            /   \.
            //           /     \.
            //          /       \.
            // const(h) int     addr byref
            //
            // <BUGNUM> VSW 318822 </BUGNUM>
            //
            // So here we decide to make the resulting type to be a native int.

            // Insert an explicit upcast if needed.
            op1 = *pOp1 = impImplicitIorI4Cast(op1, TYP_I_IMPL, fUnsigned);

            type = TYP_I_IMPL;
        }
        else
        {
            // byref - [native] int => gives a byref
            assert(op1->TypeIs(TYP_BYREF) && genActualTypeIsIntOrI(op2));

            // Insert an explicit upcast if needed.
            op2 = *pOp2 = impImplicitIorI4Cast(op2, TYP_I_IMPL, fUnsigned);

            type = TYP_BYREF;
        }
    }
    else if ((oper == GT_ADD) && (op1->TypeIs(TYP_BYREF) || op2->TypeIs(TYP_BYREF)))
    {
        // byref + [native] int => gives a byref
        // (or)
        // [native] int + byref => gives a byref

        // Only one can be a byref : byref op byref not allowed.
        assert(op1->TypeIs(TYP_BYREF) || op2->TypeIs(TYP_BYREF));
        assert(genActualTypeIsIntOrI(op1) || genActualTypeIsIntOrI(op2));

        // Insert explicit upcasts if needed.
        op1 = *pOp1 = impImplicitIorI4Cast(op1, TYP_I_IMPL, fUnsigned);
        op2 = *pOp2 = impImplicitIorI4Cast(op2, TYP_I_IMPL, fUnsigned);

        type = TYP_BYREF;
    }
#ifdef TARGET_64BIT
    else if ((genActualType(op1) == TYP_I_IMPL) || (genActualType(op2) == TYP_I_IMPL))
    {
        assert(!varTypeIsFloating(op1) && !varTypeIsFloating(op2));

        // int + long => gives long
        // long + int => gives long
        // We get this because in the IL the long isn't Int64, it's just IntPtr.
        // Insert explicit upcasts if needed.
        if (genActualType(op1) != TYP_I_IMPL)
        {
            // insert an explicit upcast
            op1 = gtNewCastNode(TYP_I_IMPL, op1, fUnsigned, TYP_I_IMPL);
        }
        else if (genActualType(op2) != TYP_I_IMPL)
        {
            // insert an explicit upcast
            op2 = gtNewCastNode(TYP_I_IMPL, op2, fUnsigned, TYP_I_IMPL);
        }

        if (opts.OptimizationEnabled())
        {
            op1 = gtFoldExpr(op1);
            op2 = gtFoldExpr(op2);
        }
        *pOp1 = op1;
        *pOp2 = op2;

        type = TYP_I_IMPL;
    }
#else  // 32-bit TARGET
    else if ((genActualType(op1) == TYP_LONG) || (genActualType(op2) == TYP_LONG))
    {
        assert(!varTypeIsFloating(op1) && !varTypeIsFloating(op2));

        // int + long => gives long
        // long + int => gives long

        type = TYP_LONG;
    }
#endif // TARGET_64BIT
    else
    {
        // int + int => gives an int
        assert((genActualType(op1) != TYP_BYREF) && (genActualType(op2) != TYP_BYREF));
        assert((genActualType(op1) == genActualType(op2)) || (varTypeIsFloating(op1) && varTypeIsFloating(op2)));

        type = genActualType(op1);

        // If both operands are TYP_FLOAT, then leave it as TYP_FLOAT. Otherwise, turn floats into doubles
        if (varTypeIsFloating(type) && (op2->TypeGet() != type))
        {
            op1 = *pOp1 = impImplicitR4orR8Cast(op1, TYP_DOUBLE);
            op2 = *pOp2 = impImplicitR4orR8Cast(op2, TYP_DOUBLE);

            type = TYP_DOUBLE;
        }
    }

    assert(TypeIs(type, TYP_BYREF, TYP_DOUBLE, TYP_FLOAT, TYP_LONG, TYP_INT));
    return type;
}

//------------------------------------------------------------------------
// impOptimizeCastClassOrIsInst: attempt to resolve a cast when jitting
//
// Arguments:
//   op1 - value to cast
//   pResolvedToken - resolved token for type to cast to
//   isCastClass - true if this is a castclass, false if isinst
//
// Return Value:
//   tree representing optimized cast, or null if no optimization possible

GenTree* Compiler::impOptimizeCastClassOrIsInst(GenTree* op1, CORINFO_RESOLVED_TOKEN* pResolvedToken, bool isCastClass)
{
    assert(op1->TypeIs(TYP_REF));

    // Don't optimize for minopts or debug codegen.
    if (opts.OptimizationDisabled())
    {
        return nullptr;
    }

    CORINFO_CLASS_HANDLE toClass = pResolvedToken->hClass;
    if (info.compCompHnd->getExactClasses(toClass, 0, nullptr) == 0)
    {
        JITDUMP("\nClass %p (%s) can never be allocated\n", dspPtr(toClass), eeGetClassName(toClass));

        if (!isCastClass)
        {
            JITDUMP("Cast will fail, optimizing to return null\n");

            // If the cast was fed by a box, we can remove that too.
            if (op1->IsBoxedValue())
            {
                JITDUMP("Also removing upstream box\n");
                gtTryRemoveBoxUpstreamEffects(op1);
            }

            if (gtTreeHasSideEffects(op1, GTF_SIDE_EFFECT))
            {
                impAppendTree(op1, CHECK_SPILL_ALL, impCurStmtDI);
            }
            return gtNewNull();
        }

        JITDUMP("Cast will always throw, but not optimizing yet\n");
    }

    // See what we know about the type of the object being cast.
    bool                 isExact   = false;
    bool                 isNonNull = false;
    CORINFO_CLASS_HANDLE fromClass = gtGetClassHandle(op1, &isExact, &isNonNull);

    if (fromClass != nullptr)
    {
        JITDUMP("\nConsidering optimization of %s from %s%p (%s) to %p (%s)\n", isCastClass ? "castclass" : "isinst",
                isExact ? "exact " : "", dspPtr(fromClass), eeGetClassName(fromClass), dspPtr(toClass),
                eeGetClassName(toClass));

        // Perhaps we know if the cast will succeed or fail.
        TypeCompareState castResult = info.compCompHnd->compareTypesForCast(fromClass, toClass);

        if (castResult == TypeCompareState::Must)
        {
            // Cast will succeed, result is simply op1.
            JITDUMP("Cast will succeed, optimizing to simply return input\n");
            return op1;
        }
        else if (castResult == TypeCompareState::MustNot)
        {
            // See if we can sharpen exactness by looking for final classes
            if (!isExact)
            {
                isExact = info.compCompHnd->isExactType(fromClass);
            }

            // Cast to exact type will fail. Handle case where we have
            // an exact type (that is, fromClass is not a subtype)
            // and we're not going to throw on failure.
            if (isExact && !isCastClass)
            {
                JITDUMP("Cast will fail, optimizing to return null\n");

                // If the cast was fed by a box, we can remove that too.
                if (op1->IsBoxedValue())
                {
                    JITDUMP("Also removing upstream box\n");
                    gtTryRemoveBoxUpstreamEffects(op1);
                }

                if (gtTreeHasSideEffects(op1, GTF_SIDE_EFFECT))
                {
                    impAppendTree(op1, CHECK_SPILL_ALL, impCurStmtDI);
                }
                return gtNewNull();
            }
            else if (isExact)
            {
                JITDUMP("Not optimizing failing castclass (yet)\n");
            }
            else
            {
                JITDUMP("Can't optimize since fromClass is inexact\n");
            }
        }
        else
        {
            JITDUMP("Result of cast unknown, must generate runtime test\n");
        }
    }
    else
    {
        JITDUMP("\nCan't optimize since fromClass is unknown\n");
    }

    return nullptr;
}

//------------------------------------------------------------------------
// impMatchIsInstBooleanConversion: Match IL to determine whether an isinst IL
// instruction is used for a simple boolean check.
//
// Arguments:
//   codeAddr - IL after the isinst
//   codeEndp - End of IL code stream
//   consumed - [out] If this function returns true, set to the number of IL
//              bytes to consume to create the boolean check
//
// Return Value:
//   True if the isinst is used as a boolean check; otherwise false.
//
// Remarks:
//   The isinst instruction is specced to return the original object refernce
//   when the type check succeeds. However, in many cases it is used strictly
//   as a boolean type check (if (x is Foo) for example). In those cases it is
//   beneficial for the JIT if we avoid creating QMARKs returning the object
//   itself which may disable some important optimization in some cases.
//
bool Compiler::impMatchIsInstBooleanConversion(const BYTE* codeAddr, const BYTE* codeEndp, int* consumed)
{
    OPCODE nextOpcode = impGetNonPrefixOpcode(codeAddr, codeEndp);
    switch (nextOpcode)
    {
        case CEE_BRFALSE:
        case CEE_BRFALSE_S:
        case CEE_BRTRUE:
        case CEE_BRTRUE_S:
            // BRFALSE/BRTRUE importation are expected to transparently handle
            // that the created tree is a TYP_INT instead of TYP_REF, so we do
            // not consume them here.
            *consumed = 0;
            return true;
        case CEE_LDNULL:
            nextOpcode = impGetNonPrefixOpcode(codeAddr + 1, codeEndp);
            if (nextOpcode == CEE_CGT_UN)
            {
                *consumed = 3;
                return true;
            }
            return false;
        default:
            return false;
    }
}

//------------------------------------------------------------------------
// impCastClassOrIsInstToTree: build and import castclass/isinst
//
// Arguments:
//   op1 - value to cast
//   op2 - type handle for type to cast to
//   pResolvedToken - resolved token from the cast operation
//   isCastClass - true if this is castclass, false means isinst
//   booleanCheck - [in, out] If true, allow creating a boolean-returning check
//                  instead of returning the object reference. Set to false if this function
//                  was not able to create a boolean check.
//
// Return Value:
//   Tree representing the cast
//
// Notes:
//   May expand into a series of runtime checks or a helper call.
//
GenTree* Compiler::impCastClassOrIsInstToTree(GenTree*                op1,
                                              GenTree*                op2,
                                              CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                              bool                    isCastClass,
                                              bool*                   booleanCheck,
                                              IL_OFFSET               ilOffset)
{
    assert(op1->TypeIs(TYP_REF));

    // Optimistically assume the jit should expand this as an inline test
    bool isClassExact = info.compCompHnd->isExactType(pResolvedToken->hClass);

    // ECMA-335 III.4.3:  If typeTok is a nullable type, Nullable<T>, it is interpreted as "boxed" T
    // We can convert constant-ish tokens of nullable to its underlying type.
    // However, when the type is shared generic parameter like Nullable<Struct<__Canon>>, the actual type will require
    // runtime lookup. It's too complex to add another level of indirection in op2, fallback to the cast helper instead.
    if (isClassExact && !eeIsSharedInst(pResolvedToken->hClass))
    {
        CORINFO_CLASS_HANDLE hClass = info.compCompHnd->getTypeForBox(pResolvedToken->hClass);
        if (hClass != pResolvedToken->hClass)
        {
            bool runtimeLookup;
            pResolvedToken->hClass = hClass;
            op2                    = impTokenToHandle(pResolvedToken, &runtimeLookup);
            assert(!runtimeLookup);
        }
    }

    const CorInfoHelpFunc helper = info.compCompHnd->getCastingHelper(pResolvedToken, isCastClass);

    bool       shouldExpandEarly = false;
    const bool tooManyLocals     = (((op1->gtFlags & GTF_GLOB_EFFECT) != 0) && lvaHaveManyLocals());
    if (isClassExact && opts.OptimizationEnabled() && !compCurBB->isRunRarely() && !tooManyLocals)
    {
        // TODO-InlineCast: Fix size regressions for these two cases if they're moved to the
        // late cast expansion path and remove this early expansion entirely.
        if (helper == CORINFO_HELP_ISINSTANCEOFCLASS)
        {
            shouldExpandEarly = true;
        }
        else if (helper == CORINFO_HELP_ISINSTANCEOFARRAY && !op2->IsIconHandle(GTF_ICON_CLASS_HDL))
        {
            shouldExpandEarly = true;
        }
    }

    if (!shouldExpandEarly)
    {
        JITDUMP("\nImporting %s as call\n", isCastClass ? "castclass" : "isinst");

        // If we CSE this class handle we prevent assertionProp from making SubType assertions
        // so instead we force the CSE logic to not consider CSE-ing this class handle.
        //
        op2->gtFlags |= GTF_DONT_CSE;
        GenTreeCall* call          = gtNewHelperCallNode(helper, TYP_REF, op2, op1);
        call->gtCastHelperILOffset = ilOffset;

        // Instrument this castclass/isinst
        if ((JitConfig.JitClassProfiling() > 0) && impIsCastHelperEligibleForClassProbe(call) && !isClassExact &&
            !compCurBB->isRunRarely())
        {
            // It doesn't make sense to instrument "x is T" or "(T)x" for shared T
            if (!eeIsSharedInst(pResolvedToken->hClass))
            {
                HandleHistogramProfileCandidateInfo* pInfo =
                    new (this, CMK_Inlining) HandleHistogramProfileCandidateInfo;
                pInfo->ilOffset                             = ilOffset;
                pInfo->probeIndex                           = info.compHandleHistogramProbeCount++;
                call->gtHandleHistogramProfileCandidateInfo = pInfo;
                compCurBB->SetFlags(BBF_HAS_HISTOGRAM_PROFILE);
            }
        }
        else
        {
            // Leave a note for fgLateCastExpand to expand this helper call
            call->gtCallMoreFlags |= GTF_CALL_M_CAST_CAN_BE_EXPANDED;
            call->gtCastHelperILOffset = ilOffset;
        }

        *booleanCheck = false;
        return call;
    }

    JITDUMP("\nExpanding isinst inline\n");

    impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("bubbling "));

    // Now we import it as two QMark nodes representing this:
    //
    //  tmp = op1;
    //  if (tmp != null) // condNull
    //  {
    //      if (tmp->pMT == op2) // condMT
    //          result = tmp;
    //      else
    //          result = null;
    //  }
    //  else
    //      result = null;
    //
    // When a boolean check is possible we create 1/0 instead of tmp/null.

    // Spill op1 if it's a complex expression
    GenTree* op1Clone;
    op1 = impCloneExpr(op1, &op1Clone, CHECK_SPILL_ALL, nullptr DEBUGARG("ISINST eval op1"));

    GenTreeOp* condNull = gtNewOperNode(GT_EQ, TYP_INT, gtClone(op1), gtNewNull());
    GenTreeOp* condMT   = gtNewOperNode(GT_NE, TYP_INT, gtNewMethodTableLookup(op1Clone), op2);

    GenTreeQmark* qmarkResult;

    if (*booleanCheck)
    {
        GenTreeQmark* qmarkMT =
            gtNewQmarkNode(TYP_INT, condMT,
                           gtNewColonNode(TYP_INT, gtNewZeroConNode(TYP_INT), gtNewOneConNode(TYP_INT)));
        qmarkResult = gtNewQmarkNode(TYP_INT, condNull, gtNewColonNode(TYP_INT, gtNewZeroConNode(TYP_INT), qmarkMT));
    }
    else
    {
        GenTreeQmark* qmarkMT = gtNewQmarkNode(TYP_REF, condMT, gtNewColonNode(TYP_REF, gtNewNull(), gtClone(op1)));
        qmarkResult           = gtNewQmarkNode(TYP_REF, condNull, gtNewColonNode(TYP_REF, gtNewNull(), qmarkMT));
    }

    // Make QMark node a top level node by spilling it.
    const unsigned result = lvaGrabTemp(true DEBUGARG("spilling qmarkNull"));
    impStoreToTemp(result, qmarkResult, CHECK_SPILL_NONE);

    if (!*booleanCheck)
    {
        // See also gtGetHelperCallClassHandle where we make the same
        // determination for the helper call variants.
        lvaSetClass(result, pResolvedToken->hClass);
    }

    return gtNewLclvNode(result, qmarkResult->TypeGet());
}

#ifndef DEBUG
#define assertImp(cond) ((void)0)
#else
#define assertImp(cond)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            const int cchAssertImpBuf = 600;                                                                           \
            char*     assertImpBuf    = (char*)_alloca(cchAssertImpBuf);                                               \
            _snprintf_s(assertImpBuf, cchAssertImpBuf, cchAssertImpBuf - 1,                                            \
                        "%s : Possibly bad IL with CEE_%s at offset %04Xh (op1=%s op2=%s stkDepth=%d)", #cond,         \
                        impCurOpcName, impCurOpcOffs, op1 ? varTypeName(op1->TypeGet()) : "NULL",                      \
                        op2 ? varTypeName(op2->TypeGet()) : "NULL", stackState.esStackDepth);                          \
            assertAbort(assertImpBuf, __FILE__, __LINE__);                                                             \
        }                                                                                                              \
    } while (0)
#endif // DEBUG

//------------------------------------------------------------------------
// impBlockIsInALoop: check if a block might be in a loop
//
// Arguments:
//    block - block to check
//
// Returns:
//    true if the block might be in a loop.
//
// Notes:
//    Conservatively correct; may return true for some blocks that are
//    not actually in loops.
//
bool Compiler::impBlockIsInALoop(BasicBlock* block)
{
    return (compIsForInlining() && impInlineInfo->iciBlock->HasFlag(BBF_BACKWARD_JUMP)) ||
           block->HasFlag(BBF_BACKWARD_JUMP);
}

//------------------------------------------------------------------------
// impMatchTaskAwaitPattern:
//   Check if a method call starts an a task await pattern that can be
//   optimized for runtime async
//
// Arguments:
//   codeAddr - IL after call[virt]
//   codeEndp - End of IL code stream
//   configVal - [out] set to 0 or 1, accordingly, if we saw ConfigureAwait(0|1)
//
// Returns:
//    true if this is an Await that we can optimize
//
bool Compiler::impMatchTaskAwaitPattern(const BYTE* codeAddr, const BYTE* codeEndp, int* configVal)
{
    // If we see the following code pattern in runtime async methods:
    //
    //    call[virt] <Method>
    //    [ OPTIONAL ]
    //       ldc.i4.0 / ldc.i4.1
    //       call[virt] <ConfigureAwait>
    //    call       <Await>
    //
    // We emit an eqivalent of:
    //
    //    call[virt] <RtMethod>
    //
    //    where "RtMethod" is the runtime-async counterpart of a Task-returning method.
    //
    //  NOTE: we could potentially check if Method is not a thunk and, in cases when we can tell,
    //        bypass this optimization. Otherwise in a non-thunk case we would be
    //        replacing the pattern with a call to a thunk, which contains roughly the same code.

    const BYTE* nextOpcode = codeAddr + sizeof(mdToken);
    // There must be enough space after ldc for {call + tk + call + tk}
    if (nextOpcode + 2 * (1 + sizeof(mdToken)) < codeEndp)
    {
        uint8_t nextOp     = getU1LittleEndian(nextOpcode);
        uint8_t nextNextOp = getU1LittleEndian(nextOpcode + 1);
        if ((nextOp != CEE_LDC_I4_0 && nextOp != CEE_LDC_I4_1) ||
            (nextNextOp != CEE_CALL && nextNextOp != CEE_CALLVIRT))
        {
            goto checkForAwait;
        }

        // check if the token after {ldc, call[virt]} is ConfigAwait
        CORINFO_RESOLVED_TOKEN nextCallTok;
        impResolveToken(nextOpcode + 2, &nextCallTok, CORINFO_TOKENKIND_Method);

        if (!eeIsIntrinsic(nextCallTok.hMethod) ||
            lookupNamedIntrinsic(nextCallTok.hMethod) != NI_System_Threading_Tasks_Task_ConfigureAwait)
        {
            goto checkForAwait;
        }

        *configVal = nextOp == CEE_LDC_I4_0 ? 0 : 1;
        // skip {ldc; call; <ConfigureAwait>}
        nextOpcode += 1 + 1 + sizeof(mdToken);
    }

checkForAwait:

    if ((nextOpcode + sizeof(mdToken) < codeEndp) && (getU1LittleEndian(nextOpcode) == CEE_CALL))
    {
        // resolve the next token
        CORINFO_RESOLVED_TOKEN nextCallTok;
        impResolveToken(nextOpcode + 1, &nextCallTok, CORINFO_TOKENKIND_Method);

        // check if it is an Await intrinsic
        if (eeIsIntrinsic(nextCallTok.hMethod) &&
            lookupNamedIntrinsic(nextCallTok.hMethod) == NI_System_Runtime_CompilerServices_AsyncHelpers_Await)
        {
            // yes, this is an Await
            return true;
        }
    }

    return false;
}

/*****************************************************************************
 *  Import the instr for the given basic block
 */
void Compiler::impImportBlockCode(BasicBlock* block)
{
#define _impResolveToken(kind) impResolveToken(codeAddr, &resolvedToken, kind)

#ifdef DEBUG

    if (verbose)
    {
        printf("\nImporting " FMT_BB " (PC=%03u) of '%s'", block->bbNum, block->bbCodeOffs, info.compFullName);
    }
#endif

    unsigned                     nxtStmtIndex = impInitBlockLineInfo();
    IL_OFFSET                    nxtStmtOffs;
    CorInfoHelpFunc              helper;
    CorInfoIsAccessAllowedResult accessAllowedResult;
    CORINFO_HELPER_DESC          calloutHelper;
    const BYTE*                  lastLoadToken = nullptr;

    /* Get the tree list started */

    impBeginTreeList();

#ifdef FEATURE_ON_STACK_REPLACEMENT

    bool enablePatchpoints =
        !opts.compDbgCode && opts.jitFlags->IsSet(JitFlags::JIT_FLAG_TIER0) && (JitConfig.TC_OnStackReplacement() > 0);

#ifdef DEBUG

    // Optionally suppress patchpoints by method hash
    //
    static ConfigMethodRange JitEnablePatchpointRange;
    JitEnablePatchpointRange.EnsureInit(JitConfig.JitEnablePatchpointRange());
    const unsigned hash    = impInlineRoot()->info.compMethodHash();
    const bool     inRange = JitEnablePatchpointRange.Contains(hash);
    enablePatchpoints &= inRange;

#endif // DEBUG

    if (enablePatchpoints)
    {
        // We don't inline at Tier0, if we do, we may need rethink our approach.
        // Could probably support inlines that don't introduce flow.
        //
        assert(!compIsForInlining());

        // OSR is not yet supported for methods with explicit tail calls.
        //
        // But we also do not have to switch these methods to be optimized, as we should be
        // able to avoid getting trapped in Tier0 code by normal call counting.
        // So instead, just suppress adding patchpoints.
        //
        if (!compTailPrefixSeen)
        {
            // We only need to add patchpoints if the method can loop.
            //
            if (compHasBackwardJump)
            {
                assert(compCanHavePatchpoints());

                // By default we use the "adaptive" strategy.
                //
                // This can create both source and target patchpoints within a given
                // loop structure, which isn't ideal, but is not incorrect. We will
                // just have some extra Tier0 overhead.
                //
                // Todo: implement support for mid-block patchpoints. If `block`
                // is truly a backedge source (and not in a handler) then we should be
                // able to find a stack empty point somewhere in the block.
                //
                const int patchpointStrategy      = JitConfig.TC_PatchpointStrategy();
                bool      addPatchpoint           = false;
                bool      mustUseTargetPatchpoint = false;

                switch (patchpointStrategy)
                {
                    default:
                    {
                        // Patchpoints at backedge sources, if possible, otherwise targets.
                        //
                        addPatchpoint           = block->HasFlag(BBF_BACKWARD_JUMP_SOURCE);
                        mustUseTargetPatchpoint = (stackState.esStackDepth != 0) || block->hasHndIndex();
                        break;
                    }

                    case 1:
                    {
                        // Patchpoints at stackempty backedge targets.
                        // Note if we have loops where the IL stack is not empty on the backedge we can't patchpoint
                        // them.
                        //
                        // We should not have allowed OSR if there were backedges in handlers.
                        //
                        assert(!block->hasHndIndex());
                        addPatchpoint = block->HasFlag(BBF_BACKWARD_JUMP_TARGET) && (stackState.esStackDepth == 0);
                        break;
                    }

                    case 2:
                    {
                        // Adaptive strategy.
                        //
                        // Patchpoints at backedge targets if there are multiple backedges,
                        // otherwise at backedge sources, if possible. Note a block can be both; if so we
                        // just need one patchpoint.
                        //
                        if (block->HasFlag(BBF_BACKWARD_JUMP_TARGET))
                        {
                            // We don't know backedge count, so just use ref count.
                            //
                            addPatchpoint = (block->bbRefs > 1) && (stackState.esStackDepth == 0);
                        }

                        if (!addPatchpoint && block->HasFlag(BBF_BACKWARD_JUMP_SOURCE))
                        {
                            addPatchpoint           = true;
                            mustUseTargetPatchpoint = (stackState.esStackDepth != 0) || block->hasHndIndex();

                            // Also force target patchpoint if target block has multiple (backedge) preds.
                            //
                            if (!mustUseTargetPatchpoint)
                            {
                                for (BasicBlock* const succBlock : block->Succs())
                                {
                                    if ((succBlock->bbNum <= block->bbNum) && (succBlock->bbRefs > 1))
                                    {
                                        mustUseTargetPatchpoint = true;
                                        break;
                                    }
                                }
                            }
                        }
                        break;
                    }
                }

                if (addPatchpoint)
                {
                    if (mustUseTargetPatchpoint)
                    {
                        // We wanted a source patchpoint, but could not have one.
                        // So, add patchpoints to the backedge targets.
                        //
                        for (BasicBlock* const succBlock : block->Succs())
                        {
                            if (succBlock->bbNum <= block->bbNum)
                            {
                                // The succBlock had better agree it's a target.
                                //
                                assert(succBlock->HasFlag(BBF_BACKWARD_JUMP_TARGET));

                                // We may already have decided to put a patchpoint in succBlock. If not, add one.
                                //
                                if (succBlock->HasFlag(BBF_PATCHPOINT))
                                {
                                    // In some cases the target may not be stack-empty at entry.
                                    // If so, we will bypass patchpoints for this backedge.
                                    //
                                    if (succBlock->bbStackDepthOnEntry() > 0)
                                    {
                                        JITDUMP("\nCan't set source patchpoint at " FMT_BB ", can't use target " FMT_BB
                                                " as it has non-empty stack on entry.\n",
                                                block->bbNum, succBlock->bbNum);
                                    }
                                    else
                                    {
                                        JITDUMP("\nCan't set source patchpoint at " FMT_BB ", using target " FMT_BB
                                                " instead\n",
                                                block->bbNum, succBlock->bbNum);

                                        assert(!succBlock->hasHndIndex());
                                        succBlock->SetFlags(BBF_PATCHPOINT);
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        assert(!block->hasHndIndex());
                        block->SetFlags(BBF_PATCHPOINT);
                    }

                    setMethodHasPatchpoint();
                }
            }
            else
            {
                // Should not see backward branch targets w/o backwards branches.
                // So if !compHasBackwardsBranch, these flags should never be set.
                //
                assert(!block->HasAnyFlag(BBF_BACKWARD_JUMP_TARGET | BBF_BACKWARD_JUMP_SOURCE));
            }
        }

#ifdef DEBUG
        // As a stress test, we can place patchpoints at the start of any block
        // that is a stack empty point and is not within a handler.
        //
        // Todo: enable for mid-block stack empty points too.
        //
        const int  offsetOSR    = JitConfig.JitOffsetOnStackReplacement();
        const int  randomOSR    = JitConfig.JitRandomOnStackReplacement();
        const bool tryOffsetOSR = offsetOSR >= 0;
        const bool tryRandomOSR = randomOSR > 0;

        if (compCanHavePatchpoints() && (tryOffsetOSR || tryRandomOSR) && (stackState.esStackDepth == 0) &&
            !block->hasHndIndex() && !block->HasFlag(BBF_PATCHPOINT))
        {
            // Block start can have a patchpoint. See if we should add one.
            //
            bool addPatchpoint = false;

            // Specific offset?
            //
            if (tryOffsetOSR)
            {
                if (impCurOpcOffs == (unsigned)offsetOSR)
                {
                    addPatchpoint = true;
                }
            }
            // Random?
            //
            else
            {
                // Reuse the random inliner's random state.
                // Note m_inlineStrategy is always created, even if we're not inlining.
                //
                CLRRandom* const random      = impInlineRoot()->m_inlineStrategy->GetRandom(randomOSR);
                const int        randomValue = (int)random->Next(100);

                addPatchpoint = (randomValue < randomOSR);
            }

            if (addPatchpoint)
            {
                block->SetFlags(BBF_PATCHPOINT);
                setMethodHasPatchpoint();
            }

            JITDUMP("\n** %s patchpoint%s added to " FMT_BB " (il offset %u)\n", tryOffsetOSR ? "offset" : "random",
                    addPatchpoint ? "" : " not", block->bbNum, impCurOpcOffs);
        }

#endif // DEBUG
    }

    // Mark stack-empty rare blocks to be considered for partial compilation.
    //
    // Ideally these are conditionally executed blocks -- if the method is going
    // to unconditionally throw, there's not as much to be gained by deferring jitting.
    // For now, we just screen out the entry bb.
    //
    // In general we might want track all the IL stack empty points so we can
    // propagate rareness back through flow and place the partial compilation patchpoints "earlier"
    // so there are fewer overall.
    //
    // Note unlike OSR, it's ok to forgo these.
    //
    if (opts.jitFlags->IsSet(JitFlags::JIT_FLAG_TIER0) && (JitConfig.TC_PartialCompilation() > 0) &&
        compCanHavePatchpoints() && !compTailPrefixSeen && (stackState.esStackDepth == 0) &&
        !block->HasFlag(BBF_PATCHPOINT) && !block->hasHndIndex())
    {
        // Is this block a good place for partial compilation?
        //
        bool addPartialCompilationPatchpoint = (block != fgFirstBB) && block->isRunRarely();

#ifdef DEBUG
        // Stress mode
        //
        const char* reason                   = "rarely run";
        const int   randomPartialCompilation = JitConfig.JitRandomPartialCompilation();
        if (randomPartialCompilation > 0)
        {
            // Reuse the random inliner's random state.
            // Note m_inlineStrategy is always created, even if we're not inlining.
            //
            CLRRandom* const random      = impInlineRoot()->m_inlineStrategy->GetRandom(randomPartialCompilation);
            const int        randomValue = (int)random->Next(100);

            addPartialCompilationPatchpoint = (randomValue < randomPartialCompilation);
            reason                          = "randomly chosen";
        }
#endif

        if (addPartialCompilationPatchpoint)
        {
            JITDUMP("\nBlock " FMT_BB " (%s) will be a partial compilation patchpoint -- not importing\n", block->bbNum,
                    reason);
            block->SetFlags(BBF_PARTIAL_COMPILATION_PATCHPOINT);
            setMethodHasPartialCompilationPatchpoint();

            // Block will no longer flow to any of its successors.
            //
            for (BasicBlock* const succ : block->Succs())
            {
                // We may have degenerate flow, make sure to fully remove
                fgRemoveAllRefPreds(succ, block);
            }

            // Change block to BBJ_THROW so we won't trigger importation of successors.
            //
            block->SetKindAndTargetEdge(BBJ_THROW);

            // If this method has a explicit generic context, the only uses of it may be in
            // the IL for this block. So assume it's used.
            //
            if (info.compMethodInfo->options &
                (CORINFO_GENERICS_CTXT_FROM_METHODDESC | CORINFO_GENERICS_CTXT_FROM_METHODTABLE))
            {
                lvaGenericsContextInUse = true;
            }

            return;
        }
    }

#endif // FEATURE_ON_STACK_REPLACEMENT

    /* Walk the opcodes that comprise the basic block */

    const BYTE* codeAddr = info.compCode + block->bbCodeOffs;
    const BYTE* codeEndp = info.compCode + block->bbCodeOffsEnd;

    IL_OFFSET opcodeOffs    = block->bbCodeOffs;
    IL_OFFSET lastSpillOffs = opcodeOffs;

    signed jmpDist;

    /* remember the start of the delegate creation sequence (used for verification) */
    const BYTE* delegateCreateStart = nullptr;

    int  prefixFlags = 0;
    bool explicitTailCall, constraintCall, readonlyCall;

    unsigned numArgs = info.compArgsCount;

    /* Now process all the opcodes in the block */

    var_types callTyp    = TYP_COUNT;
    OPCODE    prevOpcode = CEE_ILLEGAL;

    if (block->bbCatchTyp)
    {
        if (info.compStmtOffsetsImplicit & ICorDebugInfo::CALL_SITE_BOUNDARIES)
        {
            impCurStmtOffsSet(block->bbCodeOffs);
        }

        // We will spill the GT_CATCH_ARG and the input of the BB_QMARK block
        // to a temp. This is a trade off for code simplicity
        impSpillSpecialSideEff();
    }

    CORINFO_RESOLVED_TOKEN constrainedResolvedToken = {};

    while (codeAddr < codeEndp)
    {
#ifdef FEATURE_READYTORUN
        bool usingReadyToRunHelper = false;
#endif
        CORINFO_RESOLVED_TOKEN resolvedToken;
        CORINFO_CALL_INFO      callInfo;
        CORINFO_FIELD_INFO     fieldInfo;

        typeInfo tiRetVal = typeInfo(); // Default type info

        //---------------------------------------------------------------------

        /* We need to restrict the max tree depth as many of the Compiler
           functions are recursive. We do this by spilling the stack */

        if (stackState.esStackDepth)
        {
            /* Has it been a while since we last saw a non-empty stack (which
               guarantees that the tree depth isnt accumulating. */

            if ((opcodeOffs - lastSpillOffs) > MAX_TREE_SIZE && impCanSpillNow(prevOpcode))
            {
                impSpillStackEnsure();
                lastSpillOffs = opcodeOffs;
            }
        }
        else
        {
            lastSpillOffs   = opcodeOffs;
            impBoxTempInUse = false; // nothing on the stack, box temp OK to use again
        }

        /* Compute the current instr offset */

        opcodeOffs = (IL_OFFSET)(codeAddr - info.compCode);

#ifndef DEBUG
        if (opts.compDbgInfo)
#endif
        {
            nxtStmtOffs =
                (nxtStmtIndex < info.compStmtOffsetsCount) ? info.compStmtOffsets[nxtStmtIndex] : BAD_IL_OFFSET;

            /* Have we reached the next stmt boundary ? */

            if (nxtStmtOffs != BAD_IL_OFFSET && opcodeOffs >= nxtStmtOffs)
            {
                assert(nxtStmtOffs == info.compStmtOffsets[nxtStmtIndex]);

                if (stackState.esStackDepth != 0 && opts.compDbgCode)
                {
                    /* We need to provide accurate IP-mapping at this point.
                       So spill anything on the stack so that it will form
                       gtStmts with the correct stmt offset noted */

                    impSpillStackEnsure(true);
                }

                // Have we reported debug info for any tree?

                if (impCurStmtDI.IsValid() && opts.compDbgCode)
                {
                    GenTree* placeHolder = new (this, GT_NO_OP) GenTree(GT_NO_OP, TYP_VOID);
                    impAppendTree(placeHolder, CHECK_SPILL_NONE, impCurStmtDI);

                    assert(!impCurStmtDI.IsValid());
                }

                if (!impCurStmtDI.IsValid())
                {
                    /* Make sure that nxtStmtIndex is in sync with opcodeOffs.
                       If opcodeOffs has gone past nxtStmtIndex, catch up */

                    while ((nxtStmtIndex + 1) < info.compStmtOffsetsCount &&
                           info.compStmtOffsets[nxtStmtIndex + 1] <= opcodeOffs)
                    {
                        nxtStmtIndex++;
                    }

                    /* Go to the new stmt */

                    impCurStmtOffsSet(info.compStmtOffsets[nxtStmtIndex]);

                    /* Update the stmt boundary index */

                    nxtStmtIndex++;
                    assert(nxtStmtIndex <= info.compStmtOffsetsCount);

                    /* Are there any more line# entries after this one? */

                    if (nxtStmtIndex < info.compStmtOffsetsCount)
                    {
                        /* Remember where the next line# starts */

                        nxtStmtOffs = info.compStmtOffsets[nxtStmtIndex];
                    }
                    else
                    {
                        /* No more line# entries */

                        nxtStmtOffs = BAD_IL_OFFSET;
                    }
                }
            }
            else if ((info.compStmtOffsetsImplicit & ICorDebugInfo::STACK_EMPTY_BOUNDARIES) &&
                     (stackState.esStackDepth == 0))
            {
                /* At stack-empty locations, we have already added the tree to
                   the stmt list with the last offset. We just need to update
                   impCurStmtDI
                 */

                impCurStmtOffsSet(opcodeOffs);
            }
            else if ((info.compStmtOffsetsImplicit & ICorDebugInfo::CALL_SITE_BOUNDARIES) &&
                     impOpcodeIsCallSiteBoundary(prevOpcode))
            {
                /* Make sure we have a type cached */
                assert(callTyp != TYP_COUNT);

                if (callTyp == TYP_VOID)
                {
                    impCurStmtOffsSet(opcodeOffs);
                }
                else if (opts.compDbgCode)
                {
                    impSpillStackEnsure(true);
                    impCurStmtOffsSet(opcodeOffs);
                }
            }
            else if ((info.compStmtOffsetsImplicit & ICorDebugInfo::NOP_BOUNDARIES) && (prevOpcode == CEE_NOP))
            {
                if (opts.compDbgCode)
                {
                    impSpillStackEnsure(true);
                }

                impCurStmtOffsSet(opcodeOffs);
            }

            assert(!impCurStmtDI.IsValid() || (nxtStmtOffs == BAD_IL_OFFSET) ||
                   (impCurStmtDI.GetLocation().GetOffset() <= nxtStmtOffs));
        }

        CORINFO_CLASS_HANDLE clsHnd       = DUMMY_INIT(NULL);
        CORINFO_CLASS_HANDLE ldelemClsHnd = NO_CLASS_HANDLE;
        CORINFO_CLASS_HANDLE stelemClsHnd = DUMMY_INIT(NULL);

        var_types lclTyp, ovflType = TYP_UNKNOWN;
        GenTree*  op1           = DUMMY_INIT(NULL);
        GenTree*  op2           = DUMMY_INIT(NULL);
        GenTree*  newObjThisPtr = DUMMY_INIT(NULL);
        bool      uns           = DUMMY_INIT(false);
        bool      isLocal       = false;

        /* Get the next opcode and the size of its parameters */

        OPCODE opcode = (OPCODE)getU1LittleEndian(codeAddr);
        codeAddr += sizeof(int8_t);

#ifdef DEBUG
        impCurOpcOffs = (IL_OFFSET)(codeAddr - info.compCode - 1);
        JITDUMP("\n    [%2u] %3u (0x%03x) ", stackState.esStackDepth, impCurOpcOffs, impCurOpcOffs);
#endif

    DECODE_OPCODE:

        // Return if any previous code has caused inline to fail.
        if (compDonotInline())
        {
            return;
        }

        /* Get the size of additional parameters */

        signed int sz = opcodeSizes[opcode];

#ifdef DEBUG
        clsHnd  = NO_CLASS_HANDLE;
        lclTyp  = TYP_COUNT;
        callTyp = TYP_COUNT;

        impCurOpcOffs = (IL_OFFSET)(codeAddr - info.compCode - 1);
        impCurOpcName = opcodeNames[opcode];

        if (verbose && (opcode != CEE_PREFIX1))
        {
            printf("%s", impCurOpcName);
        }

        /* Use assertImp() to display the opcode */

        op1 = op2 = nullptr;
#endif

        /* See what kind of an opcode we have, then */

        unsigned mflags   = 0;
        unsigned clsFlags = 0;

        switch (opcode)
        {
            unsigned  lclNum;
            var_types type;

            GenTree*   op3;
            genTreeOps oper;

            int val;

            CORINFO_SIG_INFO     sig;
            IL_OFFSET            jmpAddr;
            bool                 ovfl, unordered, callNode;
            CORINFO_CLASS_HANDLE tokenType;

            union
            {
                int     intVal;
                float   fltVal;
                int64_t lngVal;
                double  dblVal;
            } cval;

            case CEE_PREFIX1:
                opcode     = (OPCODE)(getU1LittleEndian(codeAddr) + 256);
                opcodeOffs = (IL_OFFSET)(codeAddr - info.compCode);
                codeAddr += sizeof(int8_t);
                goto DECODE_OPCODE;

            SPILL_APPEND:
                impAppendTree(op1, CHECK_SPILL_ALL, impCurStmtDI);
                goto DONE_APPEND;

            APPEND:
                impAppendTree(op1, CHECK_SPILL_NONE, impCurStmtDI);
                goto DONE_APPEND;

            DONE_APPEND:
#ifdef DEBUG
                // Remember at which BC offset the tree was finished
                impNoteLastILoffs();
#endif
                break;

            case CEE_LDNULL:
                impPushOnStack(gtNewIconNode(0, TYP_REF), typeInfo(TYP_REF));
                break;

            case CEE_LDC_I4_M1:
            case CEE_LDC_I4_0:
            case CEE_LDC_I4_1:
            case CEE_LDC_I4_2:
            case CEE_LDC_I4_3:
            case CEE_LDC_I4_4:
            case CEE_LDC_I4_5:
            case CEE_LDC_I4_6:
            case CEE_LDC_I4_7:
            case CEE_LDC_I4_8:
                cval.intVal = (opcode - CEE_LDC_I4_0);
                assert(-1 <= cval.intVal && cval.intVal <= 8);
                goto PUSH_I4CON;

            case CEE_LDC_I4_S:
                cval.intVal = getI1LittleEndian(codeAddr);
                goto PUSH_I4CON;
            case CEE_LDC_I4:
                cval.intVal = getI4LittleEndian(codeAddr);
                goto PUSH_I4CON;
            PUSH_I4CON:
                JITDUMP(" %d", cval.intVal);
                impPushOnStack(gtNewIconNode(cval.intVal), typeInfo(TYP_INT));
                break;

            case CEE_LDC_I8:
                cval.lngVal = getI8LittleEndian(codeAddr);
                JITDUMP(" 0x%016llx", cval.lngVal);
                impPushOnStack(gtNewLconNode(cval.lngVal), typeInfo(TYP_LONG));
                break;

            case CEE_LDC_R8:
                cval.dblVal = getR8LittleEndian(codeAddr);
                JITDUMP(" %#.17g", cval.dblVal);
                impPushOnStack(gtNewDconNodeD(cval.dblVal), typeInfo(TYP_DOUBLE));
                break;

            case CEE_LDC_R4:
            {
                GenTree* dcon = gtNewDconNodeF(getR4LittleEndian(codeAddr));
                cval.dblVal   = dcon->AsDblCon()->DconValue();
                impPushOnStack(dcon, typeInfo(TYP_DOUBLE));
                JITDUMP(" %#.17g", cval.dblVal);
                break;
            }

            case CEE_LDSTR:
                val = getU4LittleEndian(codeAddr);
                JITDUMP(" %08X", val);
                impPushOnStack(gtNewSconNode(val, info.compScopeHnd), tiRetVal);
                break;

            case CEE_LDARG:
                lclNum = getU2LittleEndian(codeAddr);
                JITDUMP(" %u", lclNum);
                impLoadArg(lclNum, opcodeOffs + sz + 1);
                break;

            case CEE_LDARG_S:
                lclNum = getU1LittleEndian(codeAddr);
                JITDUMP(" %u", lclNum);
                impLoadArg(lclNum, opcodeOffs + sz + 1);
                break;

            case CEE_LDARG_0:
            case CEE_LDARG_1:
            case CEE_LDARG_2:
            case CEE_LDARG_3:
                lclNum = (opcode - CEE_LDARG_0);
                assert(lclNum >= 0 && lclNum < 4);
                impLoadArg(lclNum, opcodeOffs + sz + 1);
                break;

            case CEE_LDLOC:
                lclNum = getU2LittleEndian(codeAddr);
                JITDUMP(" %u", lclNum);
                impLoadLoc(lclNum, opcodeOffs + sz + 1);
                break;

            case CEE_LDLOC_S:
                lclNum = getU1LittleEndian(codeAddr);
                JITDUMP(" %u", lclNum);
                impLoadLoc(lclNum, opcodeOffs + sz + 1);
                break;

            case CEE_LDLOC_0:
            case CEE_LDLOC_1:
            case CEE_LDLOC_2:
            case CEE_LDLOC_3:
                lclNum = (opcode - CEE_LDLOC_0);
                assert(lclNum >= 0 && lclNum < 4);
                impLoadLoc(lclNum, opcodeOffs + sz + 1);
                break;

            case CEE_STARG:
                lclNum = getU2LittleEndian(codeAddr);
                goto STARG;

            case CEE_STARG_S:
                lclNum = getU1LittleEndian(codeAddr);
            STARG:
                JITDUMP(" %u", lclNum);

                if (compIsForInlining())
                {
                    op1 = impInlineFetchArg(impInlineInfo->inlArgInfo[lclNum], impInlineInfo->lclVarInfo[lclNum]);
                    noway_assert(op1->OperIs(GT_LCL_VAR));
                    lclNum = op1->AsLclVar()->GetLclNum();

                    goto VAR_ST_VALID;
                }

                lclNum = compMapILargNum(lclNum); // account for possible hidden param
                assertImp(lclNum < numArgs);

                if (lclNum == info.compThisArg)
                {
                    lclNum = lvaArg0Var;
                }

                // We should have seen this arg write in the prescan
                assert(lvaTable[lclNum].lvHasILStoreOp);

                goto VAR_ST;

            case CEE_STLOC:
                lclNum  = getU2LittleEndian(codeAddr);
                isLocal = true;
                JITDUMP(" %u", lclNum);
                goto LOC_ST;

            case CEE_STLOC_S:
                lclNum  = getU1LittleEndian(codeAddr);
                isLocal = true;
                JITDUMP(" %u", lclNum);
                goto LOC_ST;

            case CEE_STLOC_0:
            case CEE_STLOC_1:
            case CEE_STLOC_2:
            case CEE_STLOC_3:
                isLocal = true;
                lclNum  = (opcode - CEE_STLOC_0);
                assert(lclNum >= 0 && lclNum < 4);

            LOC_ST:
                if (compIsForInlining())
                {
                    lclTyp = impInlineInfo->lclVarInfo[lclNum + impInlineInfo->argCnt].lclTypeInfo;

                    /* Have we allocated a temp for this local? */

                    lclNum = impInlineFetchLocal(lclNum DEBUGARG("Inline stloc first use temp"));

                    goto _PopValue;
                }

                lclNum += numArgs;

            VAR_ST:

                if (lclNum >= info.compLocalsCount && lclNum != lvaArg0Var)
                {
                    BADCODE("Bad IL");
                }

            VAR_ST_VALID:

                /* if it is a struct store, make certain we don't overflow the buffer */
                assert(lclTyp != TYP_STRUCT || lvaLclStackHomeSize(lclNum) >= info.compCompHnd->getClassSize(clsHnd));

                if (lvaTable[lclNum].lvNormalizeOnLoad())
                {
                    lclTyp = lvaGetRealType(lclNum);
                }
                else
                {
                    lclTyp = lvaGetActualType(lclNum);
                }

            _PopValue:
                /* Pop the value being assigned */

                {
                    StackEntry se = impPopStack();
                    op1           = se.val;
                    tiRetVal      = se.seTypeInfo;
                }

                // Note this will downcast TYP_I_IMPL into a 32-bit Int on 64 bit (for x86 JIT compatibility).
                op1 = impImplicitIorI4Cast(op1, lclTyp);
                op1 = impImplicitR4orR8Cast(op1, lclTyp);

                // We had better assign it a value of the correct type
                assertImp(genActualType(lclTyp) == genActualType(op1->gtType) ||
                          (genActualType(lclTyp) == TYP_I_IMPL && op1->OperIs(GT_LCL_ADDR)) ||
                          (genActualType(lclTyp) == TYP_I_IMPL && (op1->TypeIs(TYP_BYREF) || op1->TypeIs(TYP_REF))) ||
                          (genActualType(op1->gtType) == TYP_I_IMPL && lclTyp == TYP_BYREF) ||
                          (varTypeIsFloating(lclTyp) && varTypeIsFloating(op1->TypeGet())) ||
                          ((genActualType(lclTyp) == TYP_BYREF) && genActualType(op1->TypeGet()) == TYP_REF));

                // If op1 is "&var" then its type is the transient "*" and it can
                // be used either as BYREF or TYP_I_IMPL.
                if (genActualType(lclTyp) == TYP_I_IMPL)
                {
                    impBashVarAddrsToI(op1);
                }

                // If this is a local and the local is a ref type, see
                // if we can improve type information based on the
                // value being assigned.
                if (isLocal && (lclTyp == TYP_REF))
                {
                    // We should have seen a stloc in our IL prescan.
                    assert(lvaTable[lclNum].lvHasILStoreOp);

                    // Is there just one place this local is defined?
                    const bool isSingleDefLocal = lvaTable[lclNum].lvSingleDef;

                    // Conservative check that there is just one
                    // definition that reaches this store.
                    const bool hasSingleReachingDef = (block->bbStackDepthOnEntry() == 0);

                    if (isSingleDefLocal && hasSingleReachingDef)
                    {
                        lvaUpdateClass(lclNum, op1, tiRetVal.GetClassHandleForObjRef());
                    }

                    // If we see a local being assigned the result of a GDV-inlineable
                    // GetEnumerator call, keep track of both the local and the call.
                    //
                    if (op1->OperIs(GT_RET_EXPR))
                    {
                        JITDUMP(".... checking for GDV returning IEnumerator<T>...\n");

                        bool                 isEnumeratorT = false;
                        GenTreeCall* const   call          = op1->AsRetExpr()->gtInlineCandidate;
                        bool                 isExact       = false;
                        bool                 isNonNull     = false;
                        CORINFO_CLASS_HANDLE retCls        = gtGetClassHandle(call, &isExact, &isNonNull);

                        if ((retCls == NO_CLASS_HANDLE) && call->IsGuardedDevirtualizationCandidate())
                        {
                            // Just check one of the GDV candidates (all should have the same original method handle)
                            //
                            InlineCandidateInfo* const inlineInfo = call->GetGDVCandidateInfo(0);
                            CORINFO_SIG_INFO           sig;
                            info.compCompHnd->getMethodSig(inlineInfo->originalMethodHandle, &sig);
                            retCls = sig.retTypeClass;
                        }

                        if ((retCls != NO_CLASS_HANDLE) && info.compCompHnd->isIntrinsicType(retCls))
                        {
                            const char* namespaceName;
                            const char* className = info.compCompHnd->getClassNameFromMetadata(retCls, &namespaceName);

                            if ((strcmp(namespaceName, "System.Collections.Generic") == 0) &&
                                (strcmp(className, "IEnumerator`1") == 0))
                            {
                                isEnumeratorT = true;
                            }
                        }

                        if (isEnumeratorT)
                        {
                            JITDUMP("V%02u value is IEnumerator<T> via GDV\n", lclNum);
                            lvaTable[lclNum].lvIsEnumerator = true;
                            JITDUMP("Flagging [%06u] for enumerator cloning via V%02u\n", dspTreeID(call), lclNum);
                            getImpEnumeratorGdvLocalMap()->Set(call, lclNum);
                            Metrics.EnumeratorGDV++;
                        }
                    }
                }

                /* Filter out simple stores to itself */

                if (op1->OperIs(GT_LCL_VAR) && lclNum == op1->AsLclVarCommon()->GetLclNum())
                {
                    if (opts.compDbgCode)
                    {
                        op1 = gtNewNothingNode();
                        goto SPILL_APPEND;
                    }
                    else
                    {
                        break;
                    }
                }

                // Stores to pinned locals can have the implicit side effect of "unpinning", so we must spill
                // things that could depend on the pin. TODO-Bug: which can actually be anything, including
                // unpinned unaliased locals, not just side-effecting trees.
                if (lvaTable[lclNum].lvPinned)
                {
                    impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("Spill before store to pinned local"));
                }

                op1 = gtNewStoreLclVarNode(lclNum, op1);

                // TODO-ASG: delete this zero-diff quirk. Requires some forward substitution work.
                op1->gtType = lclTyp;

                if (varTypeIsStruct(lclTyp))
                {
                    op1 = impStoreStruct(op1, CHECK_SPILL_ALL);
                }
                goto SPILL_APPEND;

            case CEE_LDLOCA:
                lclNum = getU2LittleEndian(codeAddr);
                goto LDLOCA;

            case CEE_LDLOCA_S:
                lclNum = getU1LittleEndian(codeAddr);
            LDLOCA:
                JITDUMP(" %u", lclNum);

                if (compIsForInlining())
                {
                    // Get the local type
                    lclTyp = impInlineInfo->lclVarInfo[lclNum + impInlineInfo->argCnt].lclTypeInfo;

                    /* Have we allocated a temp for this local? */

                    lclNum = impInlineFetchLocal(lclNum DEBUGARG("Inline ldloca(s) first use temp"));

                    assert(!lvaGetDesc(lclNum)->lvNormalizeOnLoad());
                    op1 = gtNewLclVarAddrNode(lclNum, TYP_BYREF);
                    goto _PUSH_ADRVAR;
                }

                lclNum += numArgs;
                assertImp(lclNum < info.compLocalsCount);
                goto ADRVAR;

            case CEE_LDARGA:
                lclNum = getU2LittleEndian(codeAddr);
                goto LDARGA;

            case CEE_LDARGA_S:
                lclNum = getU1LittleEndian(codeAddr);
            LDARGA:
                JITDUMP(" %u", lclNum);

                if (lclNum >= info.compILargsCount)
                {
                    BADCODE("Bad IL");
                }

                if (compIsForInlining())
                {
                    // In IL, LDARGA(_S) is used to load the byref managed pointer of struct argument,
                    // followed by a ldfld to load the field.

                    op1 = impInlineFetchArg(impInlineInfo->inlArgInfo[lclNum], impInlineInfo->lclVarInfo[lclNum]);
                    if (!op1->OperIs(GT_LCL_VAR))
                    {
                        compInlineResult->NoteFatal(InlineObservation::CALLSITE_LDARGA_NOT_LOCAL_VAR);
                        return;
                    }

                    op1 = gtNewLclAddrNode(op1->AsLclVar()->GetLclNum(), 0, TYP_BYREF);
                    goto _PUSH_ADRVAR;
                }

                lclNum = compMapILargNum(lclNum); // account for possible hidden param
                assertImp(lclNum < numArgs);

                if (lclNum == info.compThisArg)
                {
                    lclNum = lvaArg0Var;
                }

                goto ADRVAR;

            ADRVAR:
                // Note that this is supposed to create the transient type "*"
                // which may be used as a TYP_I_IMPL. However we catch places
                // where it is used as a TYP_I_IMPL and change the node if needed.
                // Thus we are pessimistic and may report byrefs in the GC info
                // where it was not absolutely needed, but doing otherwise would
                // require careful rethinking of the importer routines which use
                // the IL validity model (e. g. "impGetByRefResultType").
                op1 = gtNewLclVarAddrNode(lclNum, TYP_BYREF);

            _PUSH_ADRVAR:
                assert(op1->IsLclVarAddr());
                impPushOnStack(op1, typeInfo(TYP_BYREF));
                break;

            case CEE_ARGLIST:

                if (!info.compIsVarArgs)
                {
                    BADCODE("arglist in non-vararg method");
                }

                assertImp((info.compMethodInfo->args.callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_VARARG);

                // The ARGLIST cookie is a hidden 'last' parameter, we have already
                // adjusted the arg count cos this is like fetching the last param.
                assertImp(numArgs > 0);
                op1 = gtNewLclVarAddrNode(lvaVarargsHandleArg, TYP_BYREF);
                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_ENDFINALLY:

                if (compIsForInlining() && !opts.compInlineMethodsWithEH)
                {
                    assert(!"Shouldn't have exception handlers in the inlinee!");
                    compInlineResult->NoteFatal(InlineObservation::CALLEE_HAS_ENDFINALLY);
                    return;
                }

                if (stackState.esStackDepth > 0)
                {
                    impEvalSideEffects();
                }

                if (info.compXcptnsCount == 0)
                {
                    BADCODE("endfinally outside finally");
                }

                assert(stackState.esStackDepth == 0);

                op1 = gtNewOperNode(GT_RETFILT, TYP_VOID, nullptr);
                goto APPEND;

            case CEE_ENDFILTER:

                if (compIsForInlining() && !opts.compInlineMethodsWithEH)
                {
                    assert(!"Shouldn't have exception handlers in the inlinee!");
                    compInlineResult->NoteFatal(InlineObservation::CALLEE_HAS_ENDFILTER);
                    return;
                }

                if (!fgPgoSynthesized)
                {
                    // filters are rare
                    block->bbSetRunRarely();
                }

                if (info.compXcptnsCount == 0)
                {
                    BADCODE("endfilter outside filter");
                }

                op1 = impPopStack().val;
                assertImp(op1->TypeIs(TYP_INT));
                if (!bbInFilterILRange(block))
                {
                    BADCODE("EndFilter outside a filter handler");
                }

                /* Mark current bb as end of filter */

                assert(compCurBB->HasFlag(BBF_DONT_REMOVE));
                assert(compCurBB->KindIs(BBJ_EHFILTERRET));

                /* Mark catch handler as successor */

                op1 = gtNewOperNode(GT_RETFILT, op1->TypeGet(), op1);
                if (stackState.esStackDepth != 0)
                {
                    BADCODE("stack must be 1 on end of filter");
                }
                goto APPEND;

            case CEE_RET:
                prefixFlags &= ~PREFIX_TAILCALL; // ret without call before it
            RET:
                if (!impReturnInstruction(prefixFlags, opcode))
                {
                    return; // abort
                }
                else
                {
                    break;
                }

            case CEE_JMP:

                assert(!compIsForInlining());

                if ((info.compFlags & CORINFO_FLG_SYNCH) || block->hasTryIndex() || block->hasHndIndex())
                {
                    /* CEE_JMP does not make sense in some "protected" regions. */

                    BADCODE("Jmp not allowed in protected region");
                }

                if (opts.IsReversePInvoke())
                {
                    BADCODE("Jmp not allowed in reverse P/Invoke");
                }

                if (stackState.esStackDepth != 0)
                {
                    BADCODE("Stack must be empty after CEE_JMPs");
                }

                _impResolveToken(CORINFO_TOKENKIND_Method);

                JITDUMP(" %08X", resolvedToken.token);

                /* The signature of the target has to be identical to ours.
                   At least check that argCnt and returnType match */

                eeGetMethodSig(resolvedToken.hMethod, &sig);
                if (sig.numArgs != info.compMethodInfo->args.numArgs ||
                    sig.retType != info.compMethodInfo->args.retType ||
                    sig.callConv != info.compMethodInfo->args.callConv)
                {
                    BADCODE("Incompatible target for CEE_JMPs");
                }

                op1 = new (this, GT_JMP) GenTreeVal(GT_JMP, TYP_VOID, (size_t)resolvedToken.hMethod);

                /* Mark the basic block as being a JUMP instead of RETURN */

                block->SetFlags(BBF_HAS_JMP);

                /* Set this flag to make sure register arguments have a location assigned
                 * even if we don't use them inside the method */

                compJmpOpUsed = true;

                fgNoStructPromotion = true;

                goto APPEND;

            case CEE_LDELEMA:
            {
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                ldelemClsHnd = resolvedToken.hClass;
                lclTyp       = JITtype2varType(info.compCompHnd->asCorInfoType(ldelemClsHnd));

                // If it's a value class / pointer array, or a readonly access, we don't need a type check.
                // TODO-CQ: adapt "gtCanSkipCovariantStoreCheck" to handle "ldelema"s and call it here to
                // skip using the helper in more cases.
                if ((lclTyp != TYP_REF) || ((prefixFlags & PREFIX_READONLY) != 0))
                {
                    goto ARR_LD;
                }

                // Otherwise we need the full helper function with run-time type check
                GenTree* type = impTokenToHandle(&resolvedToken);
                if (type == nullptr)
                {
                    assert(compDonotInline());
                    return;
                }

                if (opts.OptimizationEnabled() && (gtGetArrayElementClassHandle(impStackTop(1).val) == ldelemClsHnd) &&
                    info.compCompHnd->isExactType(ldelemClsHnd))
                {
                    JITDUMP("\nldelema of T[] with T exact: skipping covariant check\n");
                    goto ARR_LD;
                }

                GenTree* index = impPopStack().val;
                GenTree* arr   = impPopStack().val;

                // The CLI Spec allows an array to be indexed by either an int32 or a native int.
                // The array helper takes a native int for array length.
                // So if we have an int, explicitly extend it to be a native int.
                index = impImplicitIorI4Cast(index, TYP_I_IMPL);
                op1   = gtNewHelperCallNode(CORINFO_HELP_LDELEMA_REF, TYP_BYREF, arr, index, type);
                impPushOnStack(op1, tiRetVal);
            }
            break;

            // ldelem for reference and value types
            case CEE_LDELEM:
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                ldelemClsHnd = resolvedToken.hClass;
                lclTyp       = TypeHandleToVarType(ldelemClsHnd);
                tiRetVal     = makeTypeInfo(ldelemClsHnd);
                goto ARR_LD;

            case CEE_LDELEM_I1:
                lclTyp = TYP_BYTE;
                goto ARR_LD;
            case CEE_LDELEM_I2:
                lclTyp = TYP_SHORT;
                goto ARR_LD;
            case CEE_LDELEM_I:
                lclTyp = TYP_I_IMPL;
                goto ARR_LD;
            case CEE_LDELEM_U4:
                lclTyp = TYP_INT;
                goto ARR_LD;
            case CEE_LDELEM_I4:
                lclTyp = TYP_INT;
                goto ARR_LD;
            case CEE_LDELEM_I8:
                lclTyp = TYP_LONG;
                goto ARR_LD;
            case CEE_LDELEM_REF:
                lclTyp = TYP_REF;
                goto ARR_LD;
            case CEE_LDELEM_R4:
                lclTyp = TYP_FLOAT;
                goto ARR_LD;
            case CEE_LDELEM_R8:
                lclTyp = TYP_DOUBLE;
                goto ARR_LD;
            case CEE_LDELEM_U1:
                lclTyp = TYP_UBYTE;
                goto ARR_LD;
            case CEE_LDELEM_U2:
                lclTyp = TYP_USHORT;
                goto ARR_LD;

            ARR_LD:

                op2 = impPopStack().val; // index
                op1 = impPopStack().val; // array
                assertImp(op1->TypeIs(TYP_REF));

                // Check for null pointer - in the inliner case we simply abort.
                if (compIsForInlining() && op1->IsIntegralConst(0))
                {
                    compInlineResult->NoteFatal(InlineObservation::CALLEE_HAS_NULL_FOR_LDELEM);
                    return;
                }

                // Mark the block as containing an index expression.

                if (op1->OperIs(GT_LCL_VAR) && op2->OperIs(GT_LCL_VAR, GT_CNS_INT, GT_ADD))
                {
                    optMethodFlags |= OMF_HAS_ARRAYREF;
                }

                op1 = gtNewArrayIndexAddr(op1, op2, lclTyp, ldelemClsHnd);

                if (opcode != CEE_LDELEMA)
                {
                    op1 = gtNewIndexIndir(op1->AsIndexAddr());
                }

                impPushOnStack(op1, tiRetVal);
                break;

            // stelem for reference and value types
            case CEE_STELEM:

                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                stelemClsHnd = resolvedToken.hClass;
                lclTyp       = TypeHandleToVarType(stelemClsHnd);

                if (lclTyp != TYP_REF)
                {
                    goto ARR_ST;
                }
                FALLTHROUGH;

            case CEE_STELEM_REF:
            {
                GenTree* value = impStackTop(0).val;
                GenTree* index = impStackTop(1).val;
                GenTree* array = impStackTop(2).val;

                if (opts.OptimizationEnabled())
                {
                    // Is this a case where we can skip the covariant store check?
                    if (gtCanSkipCovariantStoreCheck(value, array))
                    {
                        lclTyp = TYP_REF;
                        goto ARR_ST;
                    }
                }

                // Else call a helper function to do the store
                impPopStack(3);

                // The CLI Spec allows an array to be indexed by either an int32 or a native int.
                // The array helper takes a native int for array length.
                // So if we have an int, explicitly extend it to be a native int.
                index = impImplicitIorI4Cast(index, TYP_I_IMPL);
                op1   = gtNewHelperCallNode(CORINFO_HELP_ARRADDR_ST, TYP_VOID, array, index, value);
                goto SPILL_APPEND;
            }

            case CEE_STELEM_I1:
                lclTyp = TYP_BYTE;
                goto ARR_ST;
            case CEE_STELEM_I2:
                lclTyp = TYP_SHORT;
                goto ARR_ST;
            case CEE_STELEM_I:
                lclTyp = TYP_I_IMPL;
                goto ARR_ST;
            case CEE_STELEM_I4:
                lclTyp = TYP_INT;
                goto ARR_ST;
            case CEE_STELEM_I8:
                lclTyp = TYP_LONG;
                goto ARR_ST;
            case CEE_STELEM_R4:
                lclTyp = TYP_FLOAT;
                goto ARR_ST;
            case CEE_STELEM_R8:
                lclTyp = TYP_DOUBLE;
                goto ARR_ST;

            ARR_ST:
            {
                // The strict order of evaluation is 'array', 'index', 'value', range-check
                // and then store. However, the tree we create does the range-check before
                // evaluating 'value'. So to maintain strict ordering, we spill the stack.
                if ((impStackTop().val->gtFlags & GTF_SIDE_EFFECT) != 0)
                {
                    impSpillSideEffects(false,
                                        CHECK_SPILL_ALL DEBUGARG("Strict ordering of exceptions for Array store"));
                }

                // Pull the new value from the stack.
                op2 = impPopStack().val;
                impBashVarAddrsToI(op2);

                // Pull the index value.
                op1 = impPopStack().val;

                // Pull the array address.
                op3 = impPopStack().val;
                assertImp(op3->TypeIs(TYP_REF));

                // Mark the block as containing an index expression
                if (op3->OperIs(GT_LCL_VAR) && op1->OperIs(GT_LCL_VAR, GT_CNS_INT, GT_ADD))
                {
                    optMethodFlags |= OMF_HAS_ARRAYREF;
                }

                // Create the index address node.
                op1 = gtNewArrayIndexAddr(op3, op1, lclTyp, stelemClsHnd);
                op2 = impImplicitR4orR8Cast(op2, lclTyp);

                // Create the store node and append it.
                ClassLayout* layout = (lclTyp == TYP_STRUCT) ? typGetObjLayout(stelemClsHnd) : nullptr;
                op1                 = (lclTyp == TYP_STRUCT) ? gtNewStoreBlkNode(layout, op1, op2)->AsIndir()
                                                             : gtNewStoreIndNode(lclTyp, op1, op2);
                if (varTypeIsStruct(op1))
                {
                    op1 = impStoreStruct(op1, CHECK_SPILL_ALL);
                }
            }
                goto SPILL_APPEND;

            case CEE_ADD:
                oper = GT_ADD;
                goto MATH_OP2;

            case CEE_ADD_OVF:
                uns = false;
                goto ADD_OVF;
            case CEE_ADD_OVF_UN:
                uns = true;
                goto ADD_OVF;

            ADD_OVF:
                ovfl     = true;
                callNode = false;
                oper     = GT_ADD;
                goto MATH_OP2_FLAGS;

            case CEE_SUB:
                oper = GT_SUB;
                goto MATH_OP2;

            case CEE_SUB_OVF:
                uns = false;
                goto SUB_OVF;
            case CEE_SUB_OVF_UN:
                uns = true;
                goto SUB_OVF;

            SUB_OVF:
                ovfl     = true;
                callNode = false;
                oper     = GT_SUB;
                goto MATH_OP2_FLAGS;

            case CEE_MUL:
                oper = GT_MUL;
                goto MATH_MAYBE_CALL_NO_OVF;

            case CEE_MUL_OVF:
                uns = false;
                goto MUL_OVF;
            case CEE_MUL_OVF_UN:
                uns = true;
                goto MUL_OVF;

            MUL_OVF:
                ovfl = true;
                oper = GT_MUL;
                goto MATH_MAYBE_CALL_OVF;

                // Other binary math operations

            case CEE_DIV:
                oper = GT_DIV;
                goto MATH_MAYBE_CALL_NO_OVF;

            case CEE_DIV_UN:
                oper = GT_UDIV;
                goto MATH_MAYBE_CALL_NO_OVF;

            case CEE_REM:
                oper = GT_MOD;
                goto MATH_MAYBE_CALL_NO_OVF;

            case CEE_REM_UN:
                oper = GT_UMOD;
                goto MATH_MAYBE_CALL_NO_OVF;

            MATH_MAYBE_CALL_NO_OVF:
                ovfl = false;
            MATH_MAYBE_CALL_OVF:
                // Morpher has some complex logic about when to turn different
                // typed nodes on different platforms into helper calls. We
                // need to either duplicate that logic here, or just
                // pessimistically make all the nodes large enough to become
                // call nodes.  Since call nodes aren't that much larger and
                // these opcodes are infrequent enough I chose the latter.
                callNode = true;
                goto MATH_OP2_FLAGS;

            case CEE_AND:
                oper = GT_AND;
                goto MATH_OP2;
            case CEE_OR:
                oper = GT_OR;
                goto MATH_OP2;
            case CEE_XOR:
                oper = GT_XOR;
                goto MATH_OP2;

            MATH_OP2: // For default values of 'ovfl' and 'callNode'

                ovfl     = false;
                callNode = false;

            MATH_OP2_FLAGS: // If 'ovfl' and 'callNode' have already been set

                /* Pull two values and push back the result */

                op2 = impPopStack().val;
                op1 = impPopStack().val;

                /* Can't do arithmetic with references */
                assertImp(genActualType(op1->TypeGet()) != TYP_REF && genActualType(op2->TypeGet()) != TYP_REF);

                // Change both to TYP_I_IMPL (impBashVarAddrsToI won't change if its a true byref, only
                // if it is in the stack)
                impBashVarAddrsToI(op1, op2);

                type = impGetByRefResultType(oper, uns, &op1, &op2);

                assert(!ovfl || !varTypeIsFloating(op1->gtType));

                /* Special case: "int+0", "int-0", "int*1", "int/1" */

                if (op2->OperIs(GT_CNS_INT))
                {
                    if ((op2->IsIntegralConst(0) && (oper == GT_ADD || oper == GT_SUB)) ||
                        (op2->IsIntegralConst(1) && (oper == GT_MUL || oper == GT_DIV)))

                    {
                        impPushOnStack(op1, tiRetVal);
                        break;
                    }
                }

                if (callNode)
                {
                    /* These operators can later be transformed into 'GT_CALL' */

                    assert(GenTree::s_gtNodeSizes[GT_CALL] > GenTree::s_gtNodeSizes[GT_MUL]);
#ifndef TARGET_ARM
                    assert(GenTree::s_gtNodeSizes[GT_CALL] > GenTree::s_gtNodeSizes[GT_DIV]);
                    assert(GenTree::s_gtNodeSizes[GT_CALL] > GenTree::s_gtNodeSizes[GT_UDIV]);
                    assert(GenTree::s_gtNodeSizes[GT_CALL] > GenTree::s_gtNodeSizes[GT_MOD]);
                    assert(GenTree::s_gtNodeSizes[GT_CALL] > GenTree::s_gtNodeSizes[GT_UMOD]);
#endif
                    // It's tempting to use LargeOpOpcode() here, but this logic is *not* saying
                    // that we'll need to transform into a general large node, but rather specifically
                    // to a call: by doing it this way, things keep working if there are multiple sizes,
                    // and a CALL is no longer the largest.
                    // That said, as of now it *is* a large node, so we'll do this with an assert rather
                    // than an "if".
                    assert(GenTree::s_gtNodeSizes[GT_CALL] == TREE_NODE_SZ_LARGE);
                    op1 = new (this, GT_CALL) GenTreeOp(oper, type, op1, op2 DEBUGARG(/*largeNode*/ true));
                }
                else
                {
                    op1 = gtNewOperNode(oper, type, op1, op2);
                }

                /* Special case: integer/long division may throw an exception */

                if (varTypeIsIntegral(op1->TypeGet()) && op1->OperMayThrow(this))
                {
                    op1->gtFlags |= GTF_EXCEPT;
                }

                if (ovfl)
                {
                    assert(oper == GT_ADD || oper == GT_SUB || oper == GT_MUL);
                    if (ovflType != TYP_UNKNOWN)
                    {
                        op1->gtType = ovflType;
                    }
                    op1->gtFlags |= (GTF_EXCEPT | GTF_OVERFLOW);
                    if (uns)
                    {
                        op1->gtFlags |= GTF_UNSIGNED;
                    }
                }

                // Fold result, if possible.
                op1 = gtFoldExpr(op1);

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_SHL:
                oper = GT_LSH;
                goto CEE_SH_OP2;

            case CEE_SHR:
                oper = GT_RSH;
                goto CEE_SH_OP2;
            case CEE_SHR_UN:
                oper = GT_RSZ;
                goto CEE_SH_OP2;

            CEE_SH_OP2:
                op2 = impPopStack().val;
                op1 = impPopStack().val; // operand to be shifted
                impBashVarAddrsToI(op1, op2);

                type = genActualType(op1->TypeGet());
                op1  = gtNewOperNode(oper, type, op1, op2);

                // Fold result, if possible.
                op1 = gtFoldExpr(op1);

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_NOT:
                op1 = impPopStack().val;
                impBashVarAddrsToI(op1, nullptr);

                type = genActualType(op1->TypeGet());
                op1  = gtNewOperNode(GT_NOT, type, op1);

                // Fold result, if possible.
                op1 = gtFoldExpr(op1);

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_CKFINITE:
                op1  = impPopStack().val;
                type = op1->TypeGet();
                op1  = gtNewOperNode(GT_CKFINITE, type, op1);
                op1->gtFlags |= GTF_EXCEPT;

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_LEAVE:

                val     = getI4LittleEndian(codeAddr); // jump distance
                jmpAddr = (IL_OFFSET)((codeAddr - info.compCode + sizeof(int32_t)) + val);
                goto LEAVE;

            case CEE_LEAVE_S:
                val     = getI1LittleEndian(codeAddr); // jump distance
                jmpAddr = (IL_OFFSET)((codeAddr - info.compCode + sizeof(int8_t)) + val);

            LEAVE:

                if (compIsForInlining() && !opts.compInlineMethodsWithEH)
                {
                    compInlineResult->NoteFatal(InlineObservation::CALLEE_HAS_LEAVE);
                    return;
                }

                JITDUMP(" %04X", jmpAddr);
                if (!block->KindIs(BBJ_LEAVE))
                {
                    impResetLeaveBlock(block, jmpAddr);
                }

                assert(jmpAddr == block->GetTarget()->bbCodeOffs);
                impImportLeave(block);
                impNoteBranchOffs();

                break;

            case CEE_BR:
            case CEE_BR_S:
                jmpDist = (sz == 1) ? getI1LittleEndian(codeAddr) : getI4LittleEndian(codeAddr);

                if ((jmpDist == 0) && opts.DoEarlyBlockMerging())
                {
                    break; /* NOP */
                }

                impNoteBranchOffs();
                break;

            case CEE_BRTRUE:
            case CEE_BRTRUE_S:
            case CEE_BRFALSE:
            case CEE_BRFALSE_S:

                /* Pop the comparand (now there's a neat term) from the stack */
                op1 = gtFoldExpr(impPopStack().val);

                type = op1->TypeGet();

                // Per Ecma-355, brfalse and brtrue are only specified for nint, ref, and byref.
                //
                // We've historically been a bit more permissive, so here we allow
                // any type that gtNewZeroConNode can handle.
                if (!varTypeIsArithmetic(type) && !varTypeIsGC(type))
                {
                    BADCODE("invalid type for brtrue/brfalse");
                }

                if (opts.OptimizationEnabled())
                {
                    // We may have already modified `block`'s jump kind, if this is a re-importation.
                    //
                    bool jumpToNextOptimization = false;
                    if (block->KindIs(BBJ_COND) && block->TrueEdgeIs(block->GetFalseEdge()))
                    {
                        JITDUMP(FMT_BB " always branches to " FMT_BB ", changing to BBJ_ALWAYS\n", block->bbNum,
                                block->GetFalseTarget()->bbNum);
                        fgRemoveRefPred(block->GetFalseEdge());
                        block->SetKindAndTargetEdge(BBJ_ALWAYS, block->GetTrueEdge());

                        jumpToNextOptimization = true;
                    }
                    else if (block->KindIs(BBJ_ALWAYS) && block->JumpsToNext())
                    {
                        jumpToNextOptimization = true;
                    }

                    if (jumpToNextOptimization)
                    {
                        if (op1->gtFlags & GTF_GLOB_EFFECT)
                        {
                            op1 = gtUnusedValNode(op1);
                            goto SPILL_APPEND;
                        }
                        else
                        {
                            break;
                        }
                    }
                }

                if (op1->OperIsCompare())
                {
                    if (opcode == CEE_BRFALSE || opcode == CEE_BRFALSE_S)
                    {
                        // Flip the sense of the compare

                        op1 = gtReverseCond(op1);
                    }
                }
                else
                {
                    // We'll compare against an equally-sized integer 0
                    // For small types, we always compare against int
                    op2 = gtNewZeroConNode(genActualType(op1->gtType));

                    // Create the comparison operator and try to fold it
                    oper = (opcode == CEE_BRTRUE || opcode == CEE_BRTRUE_S) ? GT_NE : GT_EQ;
                    op1  = gtNewOperNode(oper, TYP_INT, op1, op2);
                }

                // fall through

            COND_JUMP:
            {
                /* Fold comparison if we can */

                op1 = gtFoldExpr(op1);

                /* Try to fold the really simple cases like 'iconst *, ifne/ifeq'*/
                /* Don't make any blocks unreachable in import only mode */

                GenTree* effectiveOp1 = op1->gtEffectiveVal();

                if (effectiveOp1->OperIs(GT_CNS_INT))
                {
                    /* gtFoldExpr() should prevent this as we don't want to make any blocks
                       unreachable under compDbgCode */
                    assert(!opts.compDbgCode);

                    // BBJ_COND: normal case
                    // BBJ_ALWAYS: this can happen if we are reimporting the block for the second time
                    assertImp(block->KindIs(BBJ_COND, BBJ_ALWAYS)); // normal case

                    if (block->KindIs(BBJ_COND))
                    {
                        bool const      isCondTrue   = effectiveOp1->AsIntCon()->gtIconVal != 0;
                        FlowEdge* const removedEdge  = isCondTrue ? block->GetFalseEdge() : block->GetTrueEdge();
                        FlowEdge* const retainedEdge = isCondTrue ? block->GetTrueEdge() : block->GetFalseEdge();

                        JITDUMP("\nThe conditional jump becomes an unconditional jump to " FMT_BB "\n",
                                retainedEdge->getDestinationBlock()->bbNum);

                        fgRemoveRefPred(removedEdge);
                        block->SetKindAndTargetEdge(BBJ_ALWAYS, retainedEdge);
                        Metrics.ImporterBranchFold++;
                        fgRepairProfileCondToUncond(block, retainedEdge, removedEdge,
                                                    &Metrics.ProfileInconsistentImporterBranchFold);
                    }

                    if (!op1->OperIs(GT_CNS_INT))
                    {
                        // Ensure we spill any side effects and don't drop them
                        op1 = gtUnusedValNode(op1);
                        goto SPILL_APPEND;
                    }
                    break;
                }

                op1 = gtNewOperNode(GT_JTRUE, TYP_VOID, op1);

                /* GT_JTRUE is handled specially for non-empty stacks. See 'addStmt'
                   in impImportBlock(block). For correct line numbers, spill stack. */

                if (opts.compDbgCode && impCurStmtDI.IsValid())
                {
                    impSpillStackEnsure(true);
                }

                goto SPILL_APPEND;
            }

            case CEE_CEQ:
                oper = GT_EQ;
                uns  = false;
                goto CMP_2_OPs;
            case CEE_CGT_UN:
                oper = GT_GT;
                uns  = true;
                goto CMP_2_OPs;
            case CEE_CGT:
                oper = GT_GT;
                uns  = false;
                goto CMP_2_OPs;
            case CEE_CLT_UN:
                oper = GT_LT;
                uns  = true;
                goto CMP_2_OPs;
            case CEE_CLT:
                oper = GT_LT;
                uns  = false;
                goto CMP_2_OPs;

            CMP_2_OPs:
                op2 = impPopStack().val;
                op1 = impPopStack().val;

                // Recognize the IL idiom of CGT_UN(op1, 0) and normalize
                // it so that downstream optimizations don't have to.
                if ((opcode == CEE_CGT_UN) && op2->IsIntegralConst(0))
                {
                    oper = GT_NE;
                    uns  = false;
                }

#ifdef TARGET_64BIT
                if (varTypeIsI(op1) && genActualTypeIsInt(op2))
                {
                    op2 = impImplicitIorI4Cast(op2, TYP_I_IMPL);
                }
                else if (varTypeIsI(op2) && genActualTypeIsInt(op1))
                {
                    op1 = impImplicitIorI4Cast(op1, TYP_I_IMPL);
                }
#endif // TARGET_64BIT

                assertImp(genActualType(op1) == genActualType(op2) || (varTypeIsI(op1) && varTypeIsI(op2)) ||
                          (varTypeIsFloating(op1) && varTypeIsFloating(op2)));

                if ((op1->TypeGet() != op2->TypeGet()) && varTypeIsFloating(op1))
                {
                    op1 = impImplicitR4orR8Cast(op1, TYP_DOUBLE);
                    op2 = impImplicitR4orR8Cast(op2, TYP_DOUBLE);
                }

                // Create the comparison node.
                op1 = gtNewOperNode(oper, TYP_INT, op1, op2);

                // TODO: setting both flags when only one is appropriate.
                if (uns)
                {
                    op1->gtFlags |= GTF_RELOP_NAN_UN | GTF_UNSIGNED;
                }

                // Fold result, if possible.
                op1 = gtFoldExpr(op1);

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_BEQ_S:
            case CEE_BEQ:
                oper = GT_EQ;
                goto CMP_2_OPs_AND_BR;

            case CEE_BGE_S:
            case CEE_BGE:
                oper = GT_GE;
                goto CMP_2_OPs_AND_BR;

            case CEE_BGE_UN_S:
            case CEE_BGE_UN:
                oper = GT_GE;
                goto CMP_2_OPs_AND_BR_UN;

            case CEE_BGT_S:
            case CEE_BGT:
                oper = GT_GT;
                goto CMP_2_OPs_AND_BR;

            case CEE_BGT_UN_S:
            case CEE_BGT_UN:
                oper = GT_GT;
                goto CMP_2_OPs_AND_BR_UN;

            case CEE_BLE_S:
            case CEE_BLE:
                oper = GT_LE;
                goto CMP_2_OPs_AND_BR;

            case CEE_BLE_UN_S:
            case CEE_BLE_UN:
                oper = GT_LE;
                goto CMP_2_OPs_AND_BR_UN;

            case CEE_BLT_S:
            case CEE_BLT:
                oper = GT_LT;
                goto CMP_2_OPs_AND_BR;

            case CEE_BLT_UN_S:
            case CEE_BLT_UN:
                oper = GT_LT;
                goto CMP_2_OPs_AND_BR_UN;

            case CEE_BNE_UN_S:
            case CEE_BNE_UN:
                oper = GT_NE;
                goto CMP_2_OPs_AND_BR_UN;

            CMP_2_OPs_AND_BR_UN:
                uns       = true;
                unordered = true;
                goto CMP_2_OPs_AND_BR_ALL;
            CMP_2_OPs_AND_BR:
                uns       = false;
                unordered = false;
                goto CMP_2_OPs_AND_BR_ALL;
            CMP_2_OPs_AND_BR_ALL:
                /* Pull two values */
                op2 = impPopStack().val;
                op1 = impPopStack().val;

#ifdef TARGET_64BIT
                // TODO-Review: this differs in the extending behavior from plain relop import. Why?
                if (op1->TypeIs(TYP_I_IMPL) && genActualTypeIsInt(op2))
                {
                    op2 = impImplicitIorI4Cast(op2, TYP_I_IMPL, uns);
                }
                else if (op2->TypeIs(TYP_I_IMPL) && genActualTypeIsInt(op1))
                {
                    op1 = impImplicitIorI4Cast(op1, TYP_I_IMPL, uns);
                }
#endif // TARGET_64BIT

                assertImp((genActualType(op1) == genActualType(op2)) || (varTypeIsI(op1) && varTypeIsI(op2)) ||
                          (varTypeIsFloating(op1) && varTypeIsFloating(op2)));

                if (opts.OptimizationEnabled())
                {
                    // We may have already modified `block`'s jump kind, if this is a re-importation.
                    //
                    bool jumpToNextOptimization = false;
                    if (block->KindIs(BBJ_COND) && block->TrueEdgeIs(block->GetFalseEdge()))
                    {
                        JITDUMP(FMT_BB " always branches to " FMT_BB ", changing to BBJ_ALWAYS\n", block->bbNum,
                                block->GetFalseTarget()->bbNum);
                        fgRemoveRefPred(block->GetFalseEdge());
                        block->SetKindAndTargetEdge(BBJ_ALWAYS, block->GetTrueEdge());

                        jumpToNextOptimization = true;
                    }
                    else if (block->KindIs(BBJ_ALWAYS) && block->JumpsToNext())
                    {
                        jumpToNextOptimization = true;
                    }

                    if (jumpToNextOptimization)
                    {
                        if (op1->gtFlags & GTF_GLOB_EFFECT)
                        {
                            impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG(
                                                           "Branch to next Optimization, op1 side effect"));
                            impAppendTree(gtUnusedValNode(op1), CHECK_SPILL_NONE, impCurStmtDI);
                        }
                        if (op2->gtFlags & GTF_GLOB_EFFECT)
                        {
                            impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG(
                                                           "Branch to next Optimization, op2 side effect"));
                            impAppendTree(gtUnusedValNode(op2), CHECK_SPILL_NONE, impCurStmtDI);
                        }

#ifdef DEBUG
                        if ((op1->gtFlags | op2->gtFlags) & GTF_GLOB_EFFECT)
                        {
                            impNoteLastILoffs();
                        }
#endif
                        break;
                    }
                }

                // We can generate an compare of different sized floating point op1 and op2.
                // We insert a cast to double.
                //
                if ((op1->TypeGet() != op2->TypeGet()) && varTypeIsFloating(op1))
                {
                    op1 = impImplicitR4orR8Cast(op1, TYP_DOUBLE);
                    op2 = impImplicitR4orR8Cast(op2, TYP_DOUBLE);
                }

                // Create and append the operator.
                op1 = gtNewOperNode(oper, TYP_INT, op1, op2);

                if (uns)
                {
                    op1->gtFlags |= GTF_UNSIGNED;
                }

                if (unordered)
                {
                    op1->gtFlags |= GTF_RELOP_NAN_UN;
                }
                goto COND_JUMP;

            case CEE_SWITCH:
                /* Pop the switch value off the stack */
                op1 = gtFoldExpr(impPopStack().val);
                assertImp(genActualTypeIsIntOrI(op1->TypeGet()));

                // Fold Switch for GT_CNS_INT
                if (opts.OptimizationEnabled() && op1->OperIs(GT_CNS_INT))
                {
                    // Find the jump target
                    size_t     switchVal = (size_t)op1->AsIntCon()->gtIconVal;
                    unsigned   jumpCnt   = block->GetSwitchTargets()->GetCaseCount();
                    FlowEdge** jumpTab   = block->GetSwitchTargets()->GetCases();
                    bool       foundVal  = false;
                    Metrics.ImporterSwitchFold++;

                    for (unsigned val = 0; val < jumpCnt; val++, jumpTab++)
                    {
                        FlowEdge* curEdge = *jumpTab;

                        assert(curEdge->getDestinationBlock()->countOfInEdges() > 0);

                        // If val matches switchVal or we are at the last entry and
                        // we never found the switch value then set the new jump dest

                        if ((val == switchVal) || (!foundVal && (val == jumpCnt - 1)))
                        {
                            // transform the basic block into a BBJ_ALWAYS
                            block->SetKindAndTargetEdge(BBJ_ALWAYS, curEdge);
                            foundVal = true;
                        }
                        else
                        {
                            // Remove 'curEdge'
                            fgRemoveRefPred(curEdge);
                        }
                    }

                    assert(foundVal);
                    JITDUMP("\nSwitch folded at " FMT_BB "\n", block->bbNum);
                    JITDUMP(FMT_BB " becomes a %s", block->bbNum, "BBJ_ALWAYS");
                    JITDUMP(" to " FMT_BB, block->GetTarget()->bbNum);
                    JITDUMP("\n");

                    if (block->hasProfileWeight())
                    {
                        // We are unlikely to be able to repair the profile.
                        // For now we don't even try.
                        //
                        JITDUMP("Profile data could not be locally repaired. Data %s inconsistent.\n",
                                fgPgoConsistent ? "is now" : "was already");

                        if (fgPgoConsistent)
                        {
                            Metrics.ProfileInconsistentImporterSwitchFold++;
                            fgPgoConsistent = false;
                        }
                    }

                    // Create a NOP node
                    op1 = gtNewNothingNode();
                }
                else
                {
                    // We can create a switch node

                    op1 = gtNewOperNode(GT_SWITCH, TYP_VOID, op1);
                }

                val = (int)getU4LittleEndian(codeAddr);
                codeAddr += 4 + val * 4; // skip over the switch-table

                goto SPILL_APPEND;

                /************************** Casting OPCODES ***************************/

            case CEE_CONV_OVF_I1:
                lclTyp = TYP_BYTE;
                goto CONV_OVF;
            case CEE_CONV_OVF_I2:
                lclTyp = TYP_SHORT;
                goto CONV_OVF;
            case CEE_CONV_OVF_I:
                lclTyp = TYP_I_IMPL;
                goto CONV_OVF;
            case CEE_CONV_OVF_I4:
                lclTyp = TYP_INT;
                goto CONV_OVF;
            case CEE_CONV_OVF_I8:
                lclTyp = TYP_LONG;
                goto CONV_OVF;

            case CEE_CONV_OVF_U1:
                lclTyp = TYP_UBYTE;
                goto CONV_OVF;
            case CEE_CONV_OVF_U2:
                lclTyp = TYP_USHORT;
                goto CONV_OVF;
            case CEE_CONV_OVF_U:
                lclTyp = TYP_U_IMPL;
                goto CONV_OVF;
            case CEE_CONV_OVF_U4:
                lclTyp = TYP_UINT;
                goto CONV_OVF;
            case CEE_CONV_OVF_U8:
                lclTyp = TYP_ULONG;
                goto CONV_OVF;

            case CEE_CONV_OVF_I1_UN:
                lclTyp = TYP_BYTE;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_I2_UN:
                lclTyp = TYP_SHORT;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_I_UN:
                lclTyp = TYP_I_IMPL;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_I4_UN:
                lclTyp = TYP_INT;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_I8_UN:
                lclTyp = TYP_LONG;
                goto CONV_OVF_UN;

            case CEE_CONV_OVF_U1_UN:
                lclTyp = TYP_UBYTE;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_U2_UN:
                lclTyp = TYP_USHORT;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_U_UN:
                lclTyp = TYP_U_IMPL;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_U4_UN:
                lclTyp = TYP_UINT;
                goto CONV_OVF_UN;
            case CEE_CONV_OVF_U8_UN:
                lclTyp = TYP_ULONG;
                goto CONV_OVF_UN;

            CONV_OVF_UN:
                uns = true;
                goto CONV_OVF_COMMON;
            CONV_OVF:
                uns = false;
                goto CONV_OVF_COMMON;

            CONV_OVF_COMMON:
                ovfl = true;
                goto _CONV;

            case CEE_CONV_I1:
                lclTyp = TYP_BYTE;
                goto CONV;
            case CEE_CONV_I2:
                lclTyp = TYP_SHORT;
                goto CONV;
            case CEE_CONV_I:
                lclTyp = TYP_I_IMPL;
                goto CONV;
            case CEE_CONV_I4:
                lclTyp = TYP_INT;
                goto CONV;
            case CEE_CONV_I8:
                lclTyp = TYP_LONG;
                goto CONV;

            case CEE_CONV_U1:
                lclTyp = TYP_UBYTE;
                goto CONV;
            case CEE_CONV_U2:
                lclTyp = TYP_USHORT;
                goto CONV;
#if (REGSIZE_BYTES == 8)
            case CEE_CONV_U:
                lclTyp = TYP_U_IMPL;
                goto CONV_UN;
#else
            case CEE_CONV_U:
                lclTyp = TYP_U_IMPL;
                goto CONV;
#endif
            case CEE_CONV_U4:
                lclTyp = TYP_UINT;
                goto CONV;
            case CEE_CONV_U8:
                lclTyp = TYP_ULONG;
                goto CONV_UN;

            case CEE_CONV_R4:
                lclTyp = TYP_FLOAT;
                goto CONV;
            case CEE_CONV_R8:
                lclTyp = TYP_DOUBLE;
                goto CONV;

            case CEE_CONV_R_UN:
                // Because there is no IL instruction conv.r4.un, compilers consistently
                // emit conv.r.un followed immediately by conv.r4 for unsigned->float casts.
                // We recognize this pattern and create the intended cast.
                // Otherwise, conv.r.un is treated as a cast to double.
                lclTyp = ((OPCODE)getU1LittleEndian(codeAddr) == CEE_CONV_R4) ? TYP_FLOAT : TYP_DOUBLE;
                goto CONV_UN;

            CONV_UN:
                uns  = true;
                ovfl = false;
                goto _CONV;

            CONV:
                uns  = false;
                ovfl = false;
                goto _CONV;

            _CONV:
                // only converts from FLOAT or DOUBLE to an integer type
                // and converts from  ULONG (or LONG on ARM) to DOUBLE are morphed to calls

                if (varTypeIsFloating(lclTyp))
                {
                    callNode = varTypeIsLong(impStackTop().val) ||
                               uns // uint->dbl gets turned into uint->long->dbl
#ifdef TARGET_64BIT
                                   // TODO-ARM64-Bug?: This was AMD64; I enabled it for ARM64 also. OK?
                                   // TYP_BYREF could be used as TYP_I_IMPL which is long.
                                   // TODO-CQ: remove this when we lower casts long/ulong --> float/double
                                   // and generate SSE2 code instead of going through helper calls.
                               || impStackTop().val->TypeIs(TYP_BYREF)
#endif
                        ;
                }
                else
                {
                    callNode = varTypeIsFloating(impStackTop().val->TypeGet());
                }

                op1 = impPopStack().val;

                impBashVarAddrsToI(op1);

                // Casts from floating point types must not have GTF_UNSIGNED set.
                if (varTypeIsFloating(op1))
                {
                    uns = false;
                }

                // At this point uns, ovf, callNode are all set.

                if (varTypeIsSmall(lclTyp) && !ovfl && op1->TypeIs(TYP_INT) && op1->OperIs(GT_AND))
                {
                    op2 = op1->AsOp()->gtOp2;

                    if (op2->OperIs(GT_CNS_INT))
                    {
                        ssize_t ival = op2->AsIntCon()->gtIconVal;
                        ssize_t mask, umask;

                        switch (lclTyp)
                        {
                            case TYP_BYTE:
                            case TYP_UBYTE:
                                mask  = 0x00FF;
                                umask = 0x007F;
                                break;
                            case TYP_USHORT:
                            case TYP_SHORT:
                                mask  = 0xFFFF;
                                umask = 0x7FFF;
                                break;

                            default:
                                assert(!"unexpected type");
                                return;
                        }

                        if (((ival & umask) == ival) || ((ival & mask) == ival && uns))
                        {
                            /* Toss the cast, it's a waste of time */

                            impPushOnStack(op1, tiRetVal);
                            break;
                        }
                        else if (ival == mask)
                        {
                            /* Toss the masking, it's a waste of time, since
                               we sign-extend from the small value anyways */

                            op1 = op1->AsOp()->gtOp1;
                        }
                    }
                }

                /*  The 'op2' sub-operand of a cast is the 'real' type number,
                    since the result of a cast to one of the 'small' integer
                    types is an integer.
                 */

                type = genActualType(lclTyp);

                // If this is a no-op cast, just use op1.
                if (!ovfl && (type == op1->TypeGet()) && (genTypeSize(type) == genTypeSize(lclTyp)))
                {
                    // Nothing needs to change
                }
                // Work is evidently required, add cast node
                else
                {
                    if (callNode)
                    {
                        op1 = gtNewCastNodeL(type, op1, uns, lclTyp);
                    }
                    else
                    {
                        op1 = gtNewCastNode(type, op1, uns, lclTyp);
                    }

                    if (ovfl)
                    {
                        op1->gtFlags |= (GTF_OVERFLOW | GTF_EXCEPT);
                    }

                    if (op1->gtGetOp1()->OperIsConst())
                    {
                        // Try and fold the introduced cast
                        op1 = gtFoldExprConst(op1);
                    }
                }

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_NEG:
                op1 = impPopStack().val;
                impBashVarAddrsToI(op1, nullptr);
                op1 = gtNewOperNode(GT_NEG, genActualType(op1->gtType), op1);

                // Fold result, if possible.
                op1 = gtFoldExpr(op1);

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_POP:
            {
                /* Pull the top value from the stack */
                op1 = impPopStack().val;

                /* Get hold of the type of the value being duplicated */

                lclTyp = genActualType(op1->gtType);

                /* Does the value have any side effects? */

                if ((op1->gtFlags & GTF_SIDE_EFFECT) || opts.compDbgCode)
                {
                    // Since we are throwing away the value, just normalize
                    // it to its address.  This is more efficient.

                    if (varTypeIsStruct(op1))
                    {
                        JITDUMP("\n ... CEE_POP struct ...\n");
                        DISPTREE(op1);
                        // If the value being produced comes from loading
                        // via an underlying address, just null check the address.
                        if (op1->OperIs(GT_IND, GT_BLK))
                        {
                            gtChangeOperToNullCheck(op1);
                        }
                        else
                        {
                            op1 = impGetNodeAddr(op1, CHECK_SPILL_ALL, nullptr);
                        }

                        JITDUMP("\n ... optimized to ...\n");
                        DISPTREE(op1);
                    }

                    // If op1 is non-overflow cast, throw it away since it is useless.
                    // Another reason for throwing away the useless cast is in the context of
                    // implicit tail calls when the operand of pop is GT_CAST(GT_CALL(..)).
                    // The cast gets added as part of importing GT_CALL, which gets in the way
                    // of fgMorphCall() on the forms of tail call nodes that we assert.
                    if (op1->OperIs(GT_CAST) && !op1->gtOverflow())
                    {
                        op1 = op1->AsOp()->gtOp1;
                    }

                    if (!op1->OperIs(GT_CALL))
                    {
                        if ((op1->gtFlags & GTF_SIDE_EFFECT) != 0)
                        {
                            op1 = gtUnusedValNode(op1);
                        }
                        else
                        {
                            // Can't bash to NOP here because op1 can be referenced from `currentBlock->bbEntryState`,
                            // if we ever need to reimport we need a valid LCL_VAR on it.
                            op1 = gtNewNothingNode();
                        }
                    }

                    /* Append the value to the tree list */
                    goto SPILL_APPEND;
                }
                else
                {
                    if (op1->IsBoxedValue())
                    {
                        JITDUMP("\n CEE_POP box...\n");
                        gtTryRemoveBoxUpstreamEffects(op1);
                    }
                }

                /* No side effects - just throw the <BEEP> thing away */
            }
            break;

            case CEE_DUP:
            {
                StackEntry se   = impPopStack();
                GenTree*   tree = se.val;
                tiRetVal        = se.seTypeInfo;
                op1             = tree;

                // In unoptimized code we leave the decision of
                // cloning/creating temps up to impCloneExpr, while in
                // optimized code we prefer temps except for some cases we know
                // are profitable.

                if (opts.OptimizationEnabled())
                {
                    bool clone = false;
                    // Duplicate 0 and +0.0
                    if (op1->IsIntegralConst(0) || op1->IsFloatPositiveZero())
                    {
                        clone = true;
                    }
                    // Duplicate locals and addresses of them
                    else if (op1->IsLocal())
                    {
                        clone = true;
                    }
                    else if (op1->TypeIs(TYP_BYREF, TYP_I_IMPL) && impIsAddressInLocal(op1))
                    {
                        clone = true;
                    }

                    if (clone)
                    {
                        op2 = gtCloneExpr(op1);
                    }
                    else
                    {
                        const unsigned tmpNum = lvaGrabTemp(true DEBUGARG("dup spill"));
                        impStoreToTemp(tmpNum, op1, CHECK_SPILL_ALL);
                        var_types type = genActualType(lvaTable[tmpNum].TypeGet());

                        assert(lvaTable[tmpNum].lvSingleDef == 0);
                        lvaTable[tmpNum].lvSingleDef = 1;
                        JITDUMP("Marked V%02u as a single def local\n", tmpNum);
                        // Propagate type info to the temp from the stack and the original tree
                        if (type == TYP_REF)
                        {
                            lvaSetClass(tmpNum, tree, tiRetVal.GetClassHandleForObjRef());
                        }

                        op1 = gtNewLclvNode(tmpNum, type);
                        op2 = gtNewLclvNode(tmpNum, type);
                    }
                }
                else
                {
                    op1 = impCloneExpr(op1, &op2, CHECK_SPILL_ALL, nullptr DEBUGARG("DUP instruction"));
                }

                assert(!(op1->gtFlags & GTF_GLOB_EFFECT) && !(op2->gtFlags & GTF_GLOB_EFFECT));
                impPushOnStack(op1, tiRetVal);
                impPushOnStack(op2, tiRetVal);
            }
            break;

            case CEE_STIND_I1:
                lclTyp = TYP_BYTE;
                goto STIND;
            case CEE_STIND_I2:
                lclTyp = TYP_SHORT;
                goto STIND;
            case CEE_STIND_I4:
                lclTyp = TYP_INT;
                goto STIND;
            case CEE_STIND_I8:
                lclTyp = TYP_LONG;
                goto STIND;
            case CEE_STIND_I:
                lclTyp = TYP_I_IMPL;
                goto STIND;
            case CEE_STIND_REF:
                lclTyp = TYP_REF;
                goto STIND;
            case CEE_STIND_R4:
                lclTyp = TYP_FLOAT;
                goto STIND;
            case CEE_STIND_R8:
                lclTyp = TYP_DOUBLE;
                goto STIND;

            STIND:
                op2 = impPopStack().val; // value to store

            STIND_VALUE:
                op1 = impPopStack().val; // address to store to

                // you can indirect off of a TYP_I_IMPL (if we are in C) or a BYREF
                assertImp(genActualType(op1->gtType) == TYP_I_IMPL || op1->TypeIs(TYP_BYREF));

                impBashVarAddrsToI(op1, op2);

                // Allow a downcast of op2 from TYP_I_IMPL into a 32-bit Int for x86 JIT compatibility.
                // Allow an upcast of op2 from a 32-bit Int into TYP_I_IMPL for x86 JIT compatibility.
                op2 = impImplicitIorI4Cast(op2, lclTyp);
                op2 = impImplicitR4orR8Cast(op2, lclTyp);

                if (opcode == CEE_STIND_REF)
                {
                    // STIND_REF can be used to store TYP_INT, TYP_I_IMPL, TYP_REF, or TYP_BYREF
                    assertImp(varTypeIsIntOrI(op2->gtType) || varTypeIsGC(op2->gtType));
                    lclTyp = genActualType(op2->TypeGet());
                }

// Check target type.
#ifdef DEBUG
                if (op2->TypeIs(TYP_BYREF) || lclTyp == TYP_BYREF)
                {
                    if (op2->TypeIs(TYP_BYREF))
                    {
                        assertImp(lclTyp == TYP_BYREF || lclTyp == TYP_I_IMPL);
                    }
                    else if (lclTyp == TYP_BYREF)
                    {
                        assertImp(op2->TypeIs(TYP_BYREF) || varTypeIsIntOrI(op2->gtType));
                    }
                }
                else
                {
                    assertImp(genActualType(op2->gtType) == genActualType(lclTyp) ||
                              ((lclTyp == TYP_I_IMPL) && (genActualType(op2->gtType) == TYP_INT)) ||
                              (varTypeIsFloating(op2->gtType) && varTypeIsFloating(lclTyp)));
                }
#endif

                op1 = gtNewStoreIndNode(lclTyp, op1, op2, impPrefixFlagsToIndirFlags(prefixFlags));
                goto SPILL_APPEND;

            case CEE_LDIND_I1:
                lclTyp = TYP_BYTE;
                goto LDIND;
            case CEE_LDIND_I2:
                lclTyp = TYP_SHORT;
                goto LDIND;
            case CEE_LDIND_U4:
            case CEE_LDIND_I4:
                lclTyp = TYP_INT;
                goto LDIND;
            case CEE_LDIND_I8:
                lclTyp = TYP_LONG;
                goto LDIND;
            case CEE_LDIND_REF:
                lclTyp = TYP_REF;
                goto LDIND;
            case CEE_LDIND_I:
                lclTyp = TYP_I_IMPL;
                goto LDIND;
            case CEE_LDIND_R4:
                lclTyp = TYP_FLOAT;
                goto LDIND;
            case CEE_LDIND_R8:
                lclTyp = TYP_DOUBLE;
                goto LDIND;
            case CEE_LDIND_U1:
                lclTyp = TYP_UBYTE;
                goto LDIND;
            case CEE_LDIND_U2:
                lclTyp = TYP_USHORT;
                goto LDIND;
            LDIND:

                op1 = impPopStack().val; // address to load from
                impBashVarAddrsToI(op1);

#ifdef TARGET_64BIT
                // Allow an upcast of op1 from a 32-bit Int into TYP_I_IMPL for x86 JIT compatibility
                //
                if (genActualType(op1->gtType) == TYP_INT)
                {
                    op1 = gtNewCastNode(TYP_I_IMPL, op1, false, TYP_I_IMPL);
                }
#endif

                assertImp(genActualType(op1->gtType) == TYP_I_IMPL || op1->TypeIs(TYP_BYREF));

                op1 = gtNewIndir(lclTyp, op1, impPrefixFlagsToIndirFlags(prefixFlags));
                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_UNALIGNED:

                assert(sz == 1);
                val = getU1LittleEndian(codeAddr);
                ++codeAddr;
                JITDUMP(" %u", val);
                if ((val != 1) && (val != 2) && (val != 4))
                {
                    BADCODE("Alignment unaligned. must be 1, 2, or 4");
                }

                prefixFlags |= PREFIX_UNALIGNED;

                impValidateMemoryAccessOpcode(codeAddr, codeEndp, false);

            PREFIX:
                opcode     = (OPCODE)getU1LittleEndian(codeAddr);
                opcodeOffs = (IL_OFFSET)(codeAddr - info.compCode);
                codeAddr += sizeof(int8_t);
                goto DECODE_OPCODE;

            case CEE_VOLATILE:

                prefixFlags |= PREFIX_VOLATILE;

                impValidateMemoryAccessOpcode(codeAddr, codeEndp, true);

                assert(sz == 0);
                goto PREFIX;

            case CEE_LDFTN:
            {
                // Need to do a lookup here so that we perform an access check
                // and do a NOWAY if protections are violated
                _impResolveToken(CORINFO_TOKENKIND_Method);

                JITDUMP(" %08X", resolvedToken.token);

                eeGetCallInfo(&resolvedToken, (prefixFlags & PREFIX_CONSTRAINED) ? &constrainedResolvedToken : nullptr,
                              combine(CORINFO_CALLINFO_SECURITYCHECKS, CORINFO_CALLINFO_LDFTN), &callInfo);

                // This check really only applies to intrinsic Array.Address methods
                if (callInfo.sig.callConv & CORINFO_CALLCONV_PARAMTYPE)
                {
                    NO_WAY("Currently do not support LDFTN of Parameterized functions");
                }

                // Do this before DO_LDFTN since CEE_LDVIRTFN does it on its own.
                impHandleAccessAllowed(callInfo.accessAllowed, &callInfo.callsiteCalloutHelper);

            DO_LDFTN:
                op1 = impMethodPointer(&resolvedToken, &callInfo);

                if (compDonotInline())
                {
                    return;
                }

                // Call info may have more precise information about the function than
                // the resolved token.
                mdToken constrainedToken     = prefixFlags & PREFIX_CONSTRAINED ? constrainedResolvedToken.token : 0;
                methodPointerInfo* heapToken = impAllocateMethodPointerInfo(resolvedToken, constrainedToken);
                assert(callInfo.hMethod != nullptr);
                heapToken->m_token.hMethod = callInfo.hMethod;
                impPushOnStack(op1, typeInfo(heapToken));

                break;
            }

            case CEE_LDVIRTFTN:
            {
                /* Get the method token */

                _impResolveToken(CORINFO_TOKENKIND_Method);

                JITDUMP(" %08X", resolvedToken.token);

                eeGetCallInfo(&resolvedToken, nullptr /* constraint typeRef */,
                              combine(combine(CORINFO_CALLINFO_SECURITYCHECKS, CORINFO_CALLINFO_LDFTN),
                                      CORINFO_CALLINFO_CALLVIRT),
                              &callInfo);

                // This check really only applies to intrinsic Array.Address methods
                if (callInfo.sig.callConv & CORINFO_CALLCONV_PARAMTYPE)
                {
                    NO_WAY("Currently do not support LDFTN of Parameterized functions");
                }

                mflags = callInfo.methodFlags;

                impHandleAccessAllowed(callInfo.accessAllowed, &callInfo.callsiteCalloutHelper);

                if (compIsForInlining())
                {
                    if (mflags & (CORINFO_FLG_FINAL | CORINFO_FLG_STATIC) || !(mflags & CORINFO_FLG_VIRTUAL))
                    {
                        compInlineResult->NoteFatal(InlineObservation::CALLSITE_LDVIRTFN_ON_NON_VIRTUAL);
                        return;
                    }
                }

                CORINFO_SIG_INFO& ftnSig = callInfo.sig;

                /* Get the object-ref */
                op1 = impPopStack().val;
                assertImp(op1->TypeIs(TYP_REF));

                if (IsAot())
                {
                    if (callInfo.kind != CORINFO_VIRTUALCALL_LDVIRTFTN)
                    {
                        if (op1->gtFlags & GTF_SIDE_EFFECT)
                        {
                            op1 = gtUnusedValNode(op1);
                            impAppendTree(op1, CHECK_SPILL_ALL, impCurStmtDI);
                        }
                        goto DO_LDFTN;
                    }
                }
                else if (mflags & (CORINFO_FLG_FINAL | CORINFO_FLG_STATIC) || !(mflags & CORINFO_FLG_VIRTUAL))
                {
                    if (op1->gtFlags & GTF_SIDE_EFFECT)
                    {
                        op1 = gtUnusedValNode(op1);
                        impAppendTree(op1, CHECK_SPILL_ALL, impCurStmtDI);
                    }
                    goto DO_LDFTN;
                }

                GenTree* fptr = impImportLdvirtftn(op1, &resolvedToken, &callInfo);
                if (compDonotInline())
                {
                    return;
                }

                methodPointerInfo* heapToken = impAllocateMethodPointerInfo(resolvedToken, 0);

                assert(heapToken->m_token.tokenType == CORINFO_TOKENKIND_Method);
                assert(callInfo.hMethod != nullptr);

                heapToken->m_token.tokenType = CORINFO_TOKENKIND_Ldvirtftn;
                heapToken->m_token.hMethod   = callInfo.hMethod;
                impPushOnStack(fptr, typeInfo(heapToken));

                break;
            }

            case CEE_CONSTRAINED:

                assertImp(sz == sizeof(unsigned));
                impResolveToken(codeAddr, &constrainedResolvedToken, CORINFO_TOKENKIND_Constrained);
                codeAddr += sizeof(unsigned); // prefix instructions must increment codeAddr manually
                JITDUMP(" (%08X) ", constrainedResolvedToken.token);

                prefixFlags |= PREFIX_CONSTRAINED;

                {
                    OPCODE actualOpcode = impGetNonPrefixOpcode(codeAddr, codeEndp);
                    if (actualOpcode != CEE_CALLVIRT && actualOpcode != CEE_CALL && actualOpcode != CEE_LDFTN)
                    {
                        BADCODE("constrained. has to be followed by callvirt, call or ldftn");
                    }
                }

                goto PREFIX;

            case CEE_READONLY:
                JITDUMP(" readonly.");

                prefixFlags |= PREFIX_READONLY;

                {
                    OPCODE actualOpcode = impGetNonPrefixOpcode(codeAddr, codeEndp);
                    if (actualOpcode != CEE_LDELEMA && !impOpcodeIsCallOpcode(actualOpcode))
                    {
                        BADCODE("readonly. has to be followed by ldelema or call");
                    }
                }

                assert(sz == 0);
                goto PREFIX;

            case CEE_TAILCALL:
                JITDUMP(" tail.");

                prefixFlags |= PREFIX_TAILCALL_EXPLICIT;

                {
                    OPCODE actualOpcode = impGetNonPrefixOpcode(codeAddr, codeEndp);
                    if (!impOpcodeIsCallOpcode(actualOpcode))
                    {
                        BADCODE("tailcall. has to be followed by call, callvirt or calli");
                    }
                }
                assert(sz == 0);
                goto PREFIX;

            case CEE_NEWOBJ:

                /* Since we will implicitly insert newObjThisPtr at the start of the
                   argument list, spill any GTF_ORDER_SIDEEFF */
                impSpillSpecialSideEff();

                /* NEWOBJ does not respond to TAIL */
                prefixFlags &= ~PREFIX_TAILCALL_EXPLICIT;

                /* NEWOBJ does not respond to CONSTRAINED */
                prefixFlags &= ~PREFIX_CONSTRAINED;

                _impResolveToken(CORINFO_TOKENKIND_NewObj);

                eeGetCallInfo(&resolvedToken, nullptr /* constraint typeRef*/,
                              combine(CORINFO_CALLINFO_SECURITYCHECKS, CORINFO_CALLINFO_ALLOWINSTPARAM), &callInfo);

                mflags = callInfo.methodFlags;

                if ((mflags & (CORINFO_FLG_STATIC | CORINFO_FLG_ABSTRACT)) != 0)
                {
                    BADCODE("newobj on static or abstract method");
                }

                // Insert the security callout before any actual code is generated
                impHandleAccessAllowed(callInfo.accessAllowed, &callInfo.callsiteCalloutHelper);

                // There are three different cases for new.
                // Object size is variable (depends on arguments).
                //      1) Object is an array (arrays treated specially by the EE)
                //      2) Object is some other variable sized object (e.g. String)
                //      3) Class Size can be determined beforehand (normal case)
                // In the first case, we need to call a NEWOBJ helper (multinewarray).
                // In the second case we call the constructor with a '0' this pointer.
                // In the third case we alloc the memory, then call the constructor.

                clsFlags = callInfo.classFlags;
                if (clsFlags & CORINFO_FLG_ARRAY)
                {
                    // Arrays need to call the NEWOBJ helper.
                    assertImp(clsFlags & CORINFO_FLG_VAROBJSIZE);

                    impImportNewObjArray(&resolvedToken, &callInfo);
                    if (compDonotInline())
                    {
                        return;
                    }

                    callTyp = TYP_REF;
                    break;
                }
                // At present this can only be String
                else if (clsFlags & CORINFO_FLG_VAROBJSIZE)
                {
                    // Skip this thisPtr argument
                    newObjThisPtr = nullptr;

                    /* Remember that this basic block contains 'new' of an object */
                    block->SetFlags(BBF_HAS_NEWOBJ);
                    optMethodFlags |= OMF_HAS_NEWOBJ;
                }
                else
                {
                    // This is the normal case where the size of the object is
                    // fixed.  Allocate the memory and call the constructor.

                    // Note: We cannot add a peep to avoid use of temp here
                    // because we don't have enough interference info to detect when
                    // sources and destination interfere, example: s = new S(ref);

                    // TODO: We find the correct place to introduce a general
                    // reverse copy prop for struct return values from newobj or
                    // any function returning structs.

                    /* get a temporary for the new object */
                    lclNum = lvaGrabTemp(true DEBUGARG("NewObj constructor temp"));
                    if (compDonotInline())
                    {
                        // Fail fast if lvaGrabTemp fails with CALLSITE_TOO_MANY_LOCALS.
                        assert(compInlineResult->GetObservation() == InlineObservation::CALLSITE_TOO_MANY_LOCALS);
                        return;
                    }

                    // In the value class case we only need clsHnd for size calcs.
                    //
                    // The lookup of the code pointer will be handled by CALL in this case
                    if (clsFlags & CORINFO_FLG_VALUECLASS)
                    {
                        if (compIsForInlining())
                        {
                            // If value class has GC fields, inform the inliner. It may choose to
                            // bail out on the inline.
                            DWORD typeFlags = info.compCompHnd->getClassAttribs(resolvedToken.hClass);
                            if ((typeFlags & CORINFO_FLG_CONTAINS_GC_PTR) != 0)
                            {
                                compInlineResult->Note(InlineObservation::CALLEE_HAS_GC_STRUCT);
                                if (compInlineResult->IsFailure())
                                {
                                    return;
                                }

                                // Do further notification in the case where the call site is rare;
                                // some policies do not track the relative hotness of call sites for
                                // "always" inline cases.
                                if (impInlineInfo->iciBlock->isRunRarely())
                                {
                                    compInlineResult->Note(InlineObservation::CALLSITE_RARE_GC_STRUCT);
                                    if (compInlineResult->IsFailure())
                                    {
                                        return;
                                    }
                                }
                            }
                        }

                        CorInfoType jitTyp = info.compCompHnd->asCorInfoType(resolvedToken.hClass);

                        if (impIsPrimitive(jitTyp))
                        {
                            lvaTable[lclNum].lvType = JITtype2varType(jitTyp);
                        }
                        else
                        {
                            // The local variable itself is the allocated space.
                            // Here we need unsafe value cls check, since the address of struct is taken for further use
                            // and potentially exploitable.
                            lvaSetStruct(lclNum, resolvedToken.hClass, true /* unsafe value cls check */);
                        }

                        bool bbInALoop  = impBlockIsInALoop(block);
                        bool bbIsReturn = block->KindIs(BBJ_RETURN) &&
                                          (!compIsForInlining() || (impInlineInfo->iciBlock->KindIs(BBJ_RETURN)));
                        LclVarDsc* const lclDsc = lvaGetDesc(lclNum);
                        if (fgVarNeedsExplicitZeroInit(lclNum, bbInALoop, bbIsReturn))
                        {
                            // Append a tree to zero-out the temp
                            GenTree* newObjInit =
                                gtNewZeroConNode(lclDsc->TypeIs(TYP_STRUCT) ? TYP_INT : lclDsc->TypeGet());

                            impStoreToTemp(lclNum, newObjInit, CHECK_SPILL_NONE);
                        }
                        else
                        {
                            JITDUMP("\nSuppressing zero-init for V%02u -- expect to zero in prolog\n", lclNum);
                            lclDsc->lvSuppressedZeroInit = 1;
                            compSuppressedZeroInit       = true;
                        }

                        // The constructor may store "this", with subsequent code mutating the underlying local
                        // through the captured reference. To correctly spill the node we'll push onto the stack
                        // in such a case, we must mark the temp as potentially aliased.
                        lclDsc->lvHasLdAddrOp = true;

                        // Obtain the address of the temp
                        newObjThisPtr = gtNewLclVarAddrNode(lclNum, TYP_BYREF);
                    }
                    else
                    {
                        // If we're newing up a finalizable object, spill anything that can cause exceptions.
                        //
                        bool            hasSideEffects = false;
                        CorInfoHelpFunc newHelper =
                            info.compCompHnd->getNewHelper(resolvedToken.hClass, &hasSideEffects);

                        if (hasSideEffects)
                        {
                            JITDUMP("\nSpilling stack for finalizable newobj\n");
                            impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("finalizable newobj spill"));
                        }

                        const bool useParent = true;
                        op1                  = gtNewAllocObjNode(&resolvedToken, info.compMethodHnd, useParent);
                        if (op1 == nullptr)
                        {
                            return;
                        }

                        // Flag if this allocation happens within a method that uses the static empty
                        // pattern (if we stack allocate this object, we can optimize the empty side away)
                        //
                        if (lookupNamedIntrinsic(info.compMethodHnd) == NI_System_SZArrayHelper_GetEnumerator)
                        {
                            JITDUMP("Allocation is part of empty static pattern\n");
                            op1->gtFlags |= GTF_ALLOCOBJ_EMPTY_STATIC;
                        }

                        // If the method being imported is an inlinee, and the original call was flagged
                        // for possible enumerator cloning, flag the allocation as well.
                        //
                        if (compIsForInlining() && hasImpEnumeratorGdvLocalMap())
                        {
                            NodeToUnsignedMap* const map           = getImpEnumeratorGdvLocalMap();
                            unsigned                 enumeratorLcl = BAD_VAR_NUM;
                            GenTreeCall* const       call          = impInlineInfo->iciCall;
                            if (map->Lookup(call, &enumeratorLcl))
                            {
                                JITDUMP("Flagging [%06u] for enumerator cloning via V%02u\n", dspTreeID(op1),
                                        enumeratorLcl);
                                map->Remove(call);
                                map->Set(op1, enumeratorLcl);
                            }
                        }

                        // Remember that this basic block contains 'new' of an object
                        block->SetFlags(BBF_HAS_NEWOBJ);
                        optMethodFlags |= OMF_HAS_NEWOBJ;

                        // Append the store to the temp/local. Dont need to spill at all as
                        // we are just calling an EE-Jit helper which can only cause
                        // an (async) OutOfMemoryException.

                        // We assign the newly allocated object (by a GT_ALLOCOBJ node)
                        // to a temp. Note that the pattern "temp = allocObj" is required
                        // by ObjectAllocator phase to be able to determine GT_ALLOCOBJ nodes
                        // without exhaustive walk over all expressions.

                        impStoreToTemp(lclNum, op1, CHECK_SPILL_NONE);

                        assert(lvaTable[lclNum].lvSingleDef == 0);
                        lvaTable[lclNum].lvSingleDef = 1;
                        JITDUMP("Marked V%02u as a single def local\n", lclNum);
                        lvaSetClass(lclNum, resolvedToken.hClass, true /* is Exact */);

                        newObjThisPtr = gtNewLclvNode(lclNum, TYP_REF);
                    }
                }
                goto CALL;

            case CEE_CALLI:

                /* CALLI does not respond to CONSTRAINED */
                prefixFlags &= ~PREFIX_CONSTRAINED;

                FALLTHROUGH;

            case CEE_CALLVIRT:
            case CEE_CALL:

                // We can't call getCallInfo on the token from a CALLI, but we need it in
                // many other places.  We unfortunately embed that knowledge here.
                if (opcode != CEE_CALLI)
                {
                    bool isAwait   = false;
                    int  configVal = -1; // -1 not configured, 0/1 configured to false/true
#ifdef DEBUG
                    if (compIsAsync() && JitConfig.JitOptimizeAwait())
#else
                    if (compIsAsync())
#endif
                    {
                        if (impMatchTaskAwaitPattern(codeAddr, codeEndp, &configVal))
                        {
                            isAwait = true;
                            prefixFlags |= PREFIX_IS_TASK_AWAIT;
                            if (configVal != 0)
                            {
                                prefixFlags |= PREFIX_TASK_AWAIT_CONTINUE_ON_CAPTURED_CONTEXT;
                            }
                        }
                    }

                    if (isAwait)
                    {
                        _impResolveToken(CORINFO_TOKENKIND_Await);
                        if (resolvedToken.hMethod != NULL)
                        {
                            // There is a runtime async variant that is implicitly awaitable, just call that.
                            // if configured, skip {ldc call ConfigureAwait}
                            if (configVal >= 0)
                            {
                                codeAddr += 2 + sizeof(mdToken);
                            }

                            // Skip the call to `Await`
                            codeAddr += 1 + sizeof(mdToken);
                        }
                        else
                        {
                            // This can happen in rare cases when the Task-returning method is not a runtime Async
                            // function. For example "T M1<T>(T arg) => arg" when called with a Task argument. Treat
                            // that as a regular call that is Awaited
                            _impResolveToken(CORINFO_TOKENKIND_Method);
                        }
                    }
                    else
                    {
                        _impResolveToken(CORINFO_TOKENKIND_Method);
                    }

                    eeGetCallInfo(&resolvedToken,
                                  (prefixFlags & PREFIX_CONSTRAINED) ? &constrainedResolvedToken : nullptr,
                                  // this is how impImportCall invokes getCallInfo
                                  combine(combine(CORINFO_CALLINFO_ALLOWINSTPARAM, CORINFO_CALLINFO_SECURITYCHECKS),
                                          (opcode == CEE_CALLVIRT) ? CORINFO_CALLINFO_CALLVIRT : CORINFO_CALLINFO_NONE),
                                  &callInfo);
                }
                else
                {
                    // Suppress uninitialized use warning.
                    memset(&resolvedToken, 0, sizeof(resolvedToken));
                    memset(&callInfo, 0, sizeof(callInfo));

                    resolvedToken.token        = getU4LittleEndian(codeAddr);
                    resolvedToken.tokenContext = impTokenLookupContextHandle;
                    resolvedToken.tokenScope   = info.compScopeHnd;
                }

            CALL: // memberRef should be set.
                // newObjThisPtr should be set for CEE_NEWOBJ

                JITDUMP(" %08X", resolvedToken.token);
                constraintCall = (prefixFlags & PREFIX_CONSTRAINED) != 0;

                bool newBBcreatedForTailcallStress;
                bool passedStressModeValidation;

                newBBcreatedForTailcallStress = false;
                passedStressModeValidation    = true;

                if (compIsForInlining())
                {
                    if (compDonotInline())
                    {
                        return;
                    }
                    // We rule out inlinees with explicit tail calls in fgMakeBasicBlocks.
                    assert((prefixFlags & PREFIX_TAILCALL_EXPLICIT) == 0);
                }
#ifdef DEBUG
                else if (compTailCallStress())
                {
                    // Have we created a new BB after the "call" instruction in fgMakeBasicBlocks()?
                    // Tail call stress only recognizes call+ret patterns and forces them to be
                    // explicit tail prefixed calls.  Also fgMakeBasicBlocks() under tail call stress
                    // doesn't import 'ret' opcode following the call into the basic block containing
                    // the call instead imports it to a new basic block.  Note that fgMakeBasicBlocks()
                    // is already checking that there is an opcode following call and hence it is
                    // safe here to read next opcode without bounds check.
                    newBBcreatedForTailcallStress =
                        impOpcodeIsCallOpcode(opcode) && // Current opcode is a CALL, (not a CEE_NEWOBJ). So, don't
                                                         // make it jump to RET.
                        (OPCODE)getU1LittleEndian(codeAddr + sz) == CEE_RET; // Next opcode is a CEE_RET

                    bool hasTailPrefix = (prefixFlags & PREFIX_TAILCALL_EXPLICIT);
                    if (newBBcreatedForTailcallStress && !hasTailPrefix)
                    {
                        // Do a more detailed evaluation of legality
                        const bool passedConstraintCheck =
                            checkTailCallConstraint(opcode, &resolvedToken,
                                                    constraintCall ? &constrainedResolvedToken : nullptr);

                        // Avoid setting compHasBackwardsJump = true via tail call stress if the method cannot have
                        // patchpoints.
                        //
                        const bool mayHavePatchpoints = opts.jitFlags->IsSet(JitFlags::JIT_FLAG_TIER0) &&
                                                        (JitConfig.TC_OnStackReplacement() > 0) &&
                                                        compCanHavePatchpoints();
                        if (passedConstraintCheck && (mayHavePatchpoints || compHasBackwardJump))
                        {
                            // Now check with the runtime
                            CORINFO_METHOD_HANDLE declaredCalleeHnd = callInfo.hMethod;
                            bool                  isVirtual         = (callInfo.kind == CORINFO_VIRTUALCALL_STUB) ||
                                             (callInfo.kind == CORINFO_VIRTUALCALL_VTABLE);
                            CORINFO_METHOD_HANDLE exactCalleeHnd = isVirtual ? nullptr : declaredCalleeHnd;
                            if (info.compCompHnd->canTailCall(info.compMethodHnd, declaredCalleeHnd, exactCalleeHnd,
                                                              hasTailPrefix)) // Is it legal to do tailcall?
                            {
                                // Stress the tailcall.
                                JITDUMP(" (Tailcall stress: prefixFlags |= PREFIX_TAILCALL_EXPLICIT)");
                                prefixFlags |= PREFIX_TAILCALL_EXPLICIT | PREFIX_TAILCALL_STRESS;
                            }
                            else
                            {
                                // Runtime disallows this tail call
                                JITDUMP(" (Tailcall stress: runtime preventing tailcall)");
                                passedStressModeValidation = false;
                            }
                        }
                        else
                        {
                            // Constraints disallow this tail call
                            JITDUMP(" (Tailcall stress: constraint check failed)");
                            passedStressModeValidation = false;
                        }
                    }
                }
#endif

                // This is split up to avoid goto flow warnings.
                bool isRecursive;
                isRecursive = !compIsForInlining() && (callInfo.hMethod == info.compMethodHnd);

                // If we've already disqualified this call as a tail call under tail call stress,
                // don't consider it for implicit tail calling either.
                //
                // When not running under tail call stress, we may mark this call as an implicit
                // tail call candidate. We'll do an "equivalent" validation during impImportCall.
                //
                // Note that when running under tail call stress, a call marked as explicit
                // tail prefixed will not be considered for implicit tail calling.
                if (passedStressModeValidation &&
                    impIsImplicitTailCallCandidate(opcode, codeAddr + sz, codeEndp, prefixFlags, isRecursive))
                {
                    if (compIsForInlining())
                    {
#if FEATURE_TAILCALL_OPT_SHARED_RETURN
                        // Are we inlining at an implicit tail call site? If so the we can flag
                        // implicit tail call sites in the inline body. These call sites
                        // often end up in non BBJ_RETURN blocks, so only flag them when
                        // we're able to handle shared returns.
                        if (impInlineInfo->iciCall->IsImplicitTailCall())
                        {
                            JITDUMP("\n (Inline Implicit Tail call: prefixFlags |= PREFIX_TAILCALL_IMPLICIT)");
                            prefixFlags |= PREFIX_TAILCALL_IMPLICIT;
                        }
#endif // FEATURE_TAILCALL_OPT_SHARED_RETURN
                    }
                    else
                    {
                        JITDUMP("\n (Implicit Tail call: prefixFlags |= PREFIX_TAILCALL_IMPLICIT)");
                        prefixFlags |= PREFIX_TAILCALL_IMPLICIT;
                    }
                }

                // Treat this call as tail call for verification only if "tail" prefixed (i.e. explicit tail call).
                explicitTailCall = (prefixFlags & PREFIX_TAILCALL_EXPLICIT) != 0;
                readonlyCall     = (prefixFlags & PREFIX_READONLY) != 0;

                if (opcode != CEE_CALLI && opcode != CEE_NEWOBJ)
                {
                    // All calls and delegates need a security callout.
                    // For delegates, this is the call to the delegate constructor, not the access check on the
                    // LD(virt)FTN.
                    impHandleAccessAllowed(callInfo.accessAllowed, &callInfo.callsiteCalloutHelper);
                }

                callTyp = impImportCall(opcode, &resolvedToken, constraintCall ? &constrainedResolvedToken : nullptr,
                                        newObjThisPtr, prefixFlags, &callInfo, opcodeOffs);
                if (compDonotInline())
                {
                    // We do not check fails after lvaGrabTemp. It is covered with CoreCLR_13272 issue.
                    assert((callTyp == TYP_UNDEF) ||
                           (compInlineResult->GetObservation() == InlineObservation::CALLSITE_TOO_MANY_LOCALS));
                    return;
                }

                if (explicitTailCall || newBBcreatedForTailcallStress) // If newBBcreatedForTailcallStress is true, we
                                                                       // have created a new BB after the "call"
                // instruction in fgMakeBasicBlocks(). So we need to jump to RET regardless.
                {
                    assert(!compIsForInlining());
                    goto RET;
                }

                break;

            case CEE_LDFLD:
            case CEE_LDSFLD:
            case CEE_LDFLDA:
            case CEE_LDSFLDA:
            {
                bool isLoadAddress = (opcode == CEE_LDFLDA || opcode == CEE_LDSFLDA);
                bool isLoadStatic  = (opcode == CEE_LDSFLD || opcode == CEE_LDSFLDA);

                /* Get the CP_Fieldref index */
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Field);

                JITDUMP(" %08X", resolvedToken.token);

                GenTreeFlags indirFlags = impPrefixFlagsToIndirFlags(prefixFlags);
                int          aflags     = isLoadAddress ? CORINFO_ACCESS_ADDRESS : CORINFO_ACCESS_GET;
                GenTree*     obj        = nullptr;

                if ((opcode == CEE_LDFLD) || (opcode == CEE_LDFLDA))
                {
                    obj = impPopStack().val;

                    if (impIsThis(obj))
                    {
                        aflags |= CORINFO_ACCESS_THIS;
                    }
                }

                eeGetFieldInfo(&resolvedToken, (CORINFO_ACCESS_FLAGS)aflags, &fieldInfo);

                // Note we avoid resolving the normalized (struct) type just yet; we may not need it (for ld[s]flda).
                lclTyp = JITtype2varType(fieldInfo.fieldType);
                clsHnd = fieldInfo.structType;

                if (compIsForInlining())
                {
                    switch (fieldInfo.fieldAccessor)
                    {
                        case CORINFO_FIELD_INSTANCE_HELPER:
                        case CORINFO_FIELD_INSTANCE_ADDR_HELPER:
                        case CORINFO_FIELD_STATIC_ADDR_HELPER:
                        case CORINFO_FIELD_STATIC_TLS:
                            compInlineResult->NoteFatal(InlineObservation::CALLEE_LDFLD_NEEDS_HELPER);
                            return;

                        case CORINFO_FIELD_STATIC_READYTORUN_HELPER:
                            compInlineResult->NoteFatal(InlineObservation::CALLSITE_LDFLD_NEEDS_HELPER);
                            return;

                        default:
                            break;
                    }

                    if (!isLoadAddress && (fieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) && (lclTyp == TYP_STRUCT))
                    {
                        if ((info.compCompHnd->getTypeForPrimitiveValueClass(clsHnd) == CORINFO_TYPE_UNDEF) &&
                            !(info.compFlags & CORINFO_FLG_FORCEINLINE))
                        {
                            // Loading a static valuetype field usually will cause a JitHelper to be called
                            // for the static base. This will bloat the code.

                            // Make an exception - small getters (6 bytes of IL) returning initialized fields, e.g.:
                            //
                            //  static DateTime Foo { get; } = DateTime.Now;
                            //
                            bool isInitedFld = (opcode == CEE_LDSFLD) && (info.compILCodeSize <= 6) &&
                                               (fieldInfo.fieldFlags & CORINFO_FLG_FIELD_FINAL);
                            if (!isInitedFld)
                            {
                                compInlineResult->Note(InlineObservation::CALLEE_LDFLD_STATIC_VALUECLASS);
                                if (compInlineResult->IsFailure())
                                {
                                    return;
                                }
                            }
                        }
                    }
                }

                if (isLoadAddress)
                {
                    tiRetVal = typeInfo(TYP_BYREF);
                }
                else
                {
                    tiRetVal = makeTypeInfo(fieldInfo.fieldType, clsHnd);
                }

                impHandleAccessAllowed(fieldInfo.accessAllowed, &fieldInfo.accessCalloutHelper);

                // Raise InvalidProgramException if static load accesses non-static field
                if (isLoadStatic && ((fieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) == 0))
                {
                    BADCODE("static access on an instance field");
                }

                // We are using ldfld/a on a static field. We allow it, but need to get side-effect from obj.
                if ((fieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) && obj != nullptr)
                {
                    if (obj->gtFlags & GTF_SIDE_EFFECT)
                    {
                        obj = gtUnusedValNode(obj);
                        impAppendTree(obj, CHECK_SPILL_ALL, impCurStmtDI);
                    }
                    obj = nullptr;
                }

                bool usesHelper = false;

                switch (fieldInfo.fieldAccessor)
                {
                    case CORINFO_FIELD_INSTANCE:
#ifdef FEATURE_READYTORUN
                    case CORINFO_FIELD_INSTANCE_WITH_BASE:
#endif
                    {
                        // If the object is a struct, what we really want is
                        // for the field to operate on the address of the struct.
                        if (varTypeIsStruct(obj))
                        {
                            if (opcode != CEE_LDFLD)
                            {
                                BADCODE3("Unexpected opcode (has to be LDFLD)", ": %02X", (int)opcode);
                            }

                            // TODO-Bug?: verify if flags matter here
                            GenTreeFlags indirFlags = GTF_EMPTY;
                            obj                     = impGetNodeAddr(obj, CHECK_SPILL_ALL, &indirFlags);
                        }

                        op1 = gtNewFieldAddrNode(resolvedToken.hField, obj, fieldInfo.offset);

#ifdef FEATURE_READYTORUN
                        if (fieldInfo.fieldAccessor == CORINFO_FIELD_INSTANCE_WITH_BASE)
                        {
                            op1->AsFieldAddr()->gtFieldLookup = fieldInfo.fieldLookup;
                        }
#endif
                        if (StructHasOverlappingFields(info.compCompHnd->getClassAttribs(resolvedToken.hClass)))
                        {
                            op1->AsFieldAddr()->gtFldMayOverlap = true;
                        }

                        if (!isLoadAddress && compIsForInlining() &&
                            impInlineIsGuaranteedThisDerefBeforeAnySideEffects(nullptr, nullptr, obj,
                                                                               impInlineInfo->inlArgInfo))
                        {
                            impInlineInfo->thisDereferencedFirst = true;
                        }
                    }
                    break;

                    case CORINFO_FIELD_STATIC_TLS:
#ifdef TARGET_X86
                        // Legacy TLS access is implemented as intrinsic on x86 only
                        op1 = gtNewFieldAddrNode(TYP_I_IMPL, resolvedToken.hField, nullptr, fieldInfo.offset);
                        op1->gtFlags |= GTF_FLD_TLS; // fgMorphExpandTlsField will handle the transformation.
                        break;
#else
                        fieldInfo.fieldAccessor = CORINFO_FIELD_STATIC_ADDR_HELPER;
                        FALLTHROUGH;
#endif
                    case CORINFO_FIELD_STATIC_ADDR_HELPER:
                    case CORINFO_FIELD_INSTANCE_HELPER:
                    case CORINFO_FIELD_INSTANCE_ADDR_HELPER:
                        op1 = gtNewRefCOMfield(obj, &resolvedToken, (CORINFO_ACCESS_FLAGS)aflags, &fieldInfo, lclTyp,
                                               nullptr);
                        usesHelper = true;
                        break;

                    case CORINFO_FIELD_STATIC_TLS_MANAGED:
                        setMethodHasTlsFieldAccess();
                        FALLTHROUGH;
                    case CORINFO_FIELD_STATIC_SHARED_STATIC_HELPER:
                    case CORINFO_FIELD_STATIC_ADDRESS:
                    case CORINFO_FIELD_STATIC_RELOCATABLE:
                        // Replace static read-only fields with constant if possible
                        if ((aflags & CORINFO_ACCESS_GET) && (fieldInfo.fieldFlags & CORINFO_FLG_FIELD_FINAL))
                        {
                            GenTree* newTree = impImportStaticReadOnlyField(resolvedToken.hField, resolvedToken.hClass);

                            if (newTree != nullptr)
                            {
                                op1 = newTree;
                                goto FIELD_DONE;
                            }
                        }
                        FALLTHROUGH;

                    case CORINFO_FIELD_STATIC_RVA_ADDRESS:
                    case CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER:
                    case CORINFO_FIELD_STATIC_READYTORUN_HELPER:
                        op1 = impImportStaticFieldAddress(&resolvedToken, (CORINFO_ACCESS_FLAGS)aflags, &fieldInfo,
                                                          lclTyp, &indirFlags);
                        break;

                    case CORINFO_FIELD_INTRINSIC_ZERO:
                    {
                        assert(aflags & CORINFO_ACCESS_GET);
                        // Widen to stack type
                        lclTyp = genActualType(lclTyp);
                        op1    = gtNewIconNode(0, lclTyp);
                        goto FIELD_DONE;
                    }
                    break;

                    case CORINFO_FIELD_INTRINSIC_EMPTY_STRING:
                    {
                        assert(aflags & CORINFO_ACCESS_GET);

                        // Import String.Empty as "" (GT_CNS_STR with a fake SconCPX = 0)
                        op1 = gtNewSconNode(EMPTY_STRING_SCON, nullptr);
                        goto FIELD_DONE;
                    }
                    break;

                    case CORINFO_FIELD_INTRINSIC_ISLITTLEENDIAN:
                    {
                        assert(aflags & CORINFO_ACCESS_GET);
                        // Widen to stack type
                        lclTyp = genActualType(lclTyp);
#if BIGENDIAN
                        op1 = gtNewIconNode(0, lclTyp);
#else
                        op1 = gtNewIconNode(1, lclTyp);
#endif
                        goto FIELD_DONE;
                    }
                    break;

                    default:
                        assert(!"Unexpected fieldAccessor");
                }

                if (!isLoadAddress && !usesHelper)
                {
                    ClassLayout* layout;
                    lclTyp = TypeHandleToVarType(fieldInfo.fieldType, clsHnd, &layout);
                    op1    = (lclTyp == TYP_STRUCT) ? gtNewBlkIndir(layout, op1, indirFlags)
                                                    : gtNewIndir(lclTyp, op1, indirFlags);

                    impAnnotateFieldIndir(op1->AsIndir());
                }

                // Check if the class needs explicit initialization.
                if (fieldInfo.fieldFlags & CORINFO_FLG_FIELD_INITCLASS)
                {
                    GenTree* helperNode = impInitClass(&resolvedToken);
                    if (compDonotInline())
                    {
                        return;
                    }
                    if (helperNode != nullptr)
                    {
                        op1 = gtNewOperNode(GT_COMMA, op1->TypeGet(), helperNode, op1);
                    }
                }

            FIELD_DONE:
                impPushOnStack(op1, tiRetVal);
            }
            break;

            case CEE_STFLD:
            case CEE_STSFLD:
            {
                bool isStoreStatic = (opcode == CEE_STSFLD);

                /* Get the CP_Fieldref index */

                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Field);

                JITDUMP(" %08X", resolvedToken.token);

                GenTreeFlags indirFlags = impPrefixFlagsToIndirFlags(prefixFlags);
                int          aflags     = CORINFO_ACCESS_SET;
                GenTree*     obj        = nullptr;

                eeGetFieldInfo(&resolvedToken, (CORINFO_ACCESS_FLAGS)aflags, &fieldInfo);

                ClassLayout* layout;
                lclTyp = TypeHandleToVarType(fieldInfo.fieldType, fieldInfo.structType, &layout);

                if (compIsForInlining())
                {
                    /* Is this a 'special' (COM) field? or a TLS ref static field?, field stored int GC heap? or
                     * per-inst static? */

                    switch (fieldInfo.fieldAccessor)
                    {
                        case CORINFO_FIELD_INSTANCE_HELPER:
                        case CORINFO_FIELD_INSTANCE_ADDR_HELPER:
                        case CORINFO_FIELD_STATIC_ADDR_HELPER:
                        case CORINFO_FIELD_STATIC_TLS:
                            compInlineResult->NoteFatal(InlineObservation::CALLEE_STFLD_NEEDS_HELPER);
                            return;

                        case CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER:
                        case CORINFO_FIELD_STATIC_READYTORUN_HELPER:
                            /* We may be able to inline the field accessors in specific instantiations of generic
                             * methods */
                            compInlineResult->NoteFatal(InlineObservation::CALLSITE_STFLD_NEEDS_HELPER);
                            return;

                        default:
                            break;
                    }
                }

                impHandleAccessAllowed(fieldInfo.accessAllowed, &fieldInfo.accessCalloutHelper);

                // Check if the class needs explicit initialization.
                if (fieldInfo.fieldFlags & CORINFO_FLG_FIELD_INITCLASS)
                {
                    GenTree* helperNode = impInitClass(&resolvedToken);
                    if (compDonotInline())
                    {
                        return;
                    }
                    if (helperNode != nullptr)
                    {
                        bool isHoistable =
                            info.compCompHnd->getClassAttribs(resolvedToken.hClass) & CORINFO_FLG_BEFOREFIELDINIT;
                        unsigned checkSpill = isHoistable ? CHECK_SPILL_NONE : CHECK_SPILL_ALL;
                        impAppendTree(helperNode, checkSpill, impCurStmtDI);
                    }
                }

                // Handle the cases that might trigger type initialization
                // (and possibly need to spill the tree for the stored value)
                switch (fieldInfo.fieldAccessor)
                {
                    case CORINFO_FIELD_INSTANCE:
#ifdef FEATURE_READYTORUN
                    case CORINFO_FIELD_INSTANCE_WITH_BASE:
#endif
                    case CORINFO_FIELD_STATIC_TLS:
                    case CORINFO_FIELD_STATIC_ADDR_HELPER:
                    case CORINFO_FIELD_INSTANCE_HELPER:
                    case CORINFO_FIELD_INSTANCE_ADDR_HELPER:
                        // Nothing now - handled later
                        break;

                    case CORINFO_FIELD_STATIC_TLS_MANAGED:
                    case CORINFO_FIELD_STATIC_ADDRESS:
                    case CORINFO_FIELD_STATIC_RVA_ADDRESS:
                    case CORINFO_FIELD_STATIC_SHARED_STATIC_HELPER:
                    case CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER:
                    case CORINFO_FIELD_STATIC_READYTORUN_HELPER:
                    case CORINFO_FIELD_STATIC_RELOCATABLE:
                        bool isHoistable;
                        op1 = impImportStaticFieldAddress(&resolvedToken, (CORINFO_ACCESS_FLAGS)aflags, &fieldInfo,
                                                          lclTyp, &indirFlags, &isHoistable);

                        if (!isHoistable)
                        {
                            impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("value for stsfld with typeinit"));
                        }
                        else if (compIsAsync() && op1->TypeIs(TYP_BYREF))
                        {
                            // TODO-Async: We really only need to spill if
                            // there is a possibility of an async call in op2.
                            impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("byref address in async method"));
                        }
                        break;

                    default:
                        assert(!"Unexpected fieldAccessor");
                }

                // Pull the value from the stack.
                op2 = impPopStack().val;

                if (opcode == CEE_STFLD)
                {
                    obj = impPopStack().val;

                    if (impIsThis(obj))
                    {
                        aflags |= CORINFO_ACCESS_THIS;
                    }
                }

                // Raise InvalidProgramException if static store accesses non-static field
                if (isStoreStatic && ((fieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) == 0))
                {
                    BADCODE("static access on an instance field");
                }

                // We are using stfld on a static field.
                // We allow it, but need to eval any side-effects for obj
                if ((fieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) && obj != nullptr)
                {
                    if (obj->gtFlags & GTF_SIDE_EFFECT)
                    {
                        obj = gtUnusedValNode(obj);
                        impAppendTree(obj, CHECK_SPILL_ALL, impCurStmtDI);
                    }
                    obj = nullptr;
                }

                // Handle the cases that use the stored value (obj).
                // Conveniently these don't trigger type initialization, so there aren't
                // any ordering issues between it and the tree for the stored value.
                switch (fieldInfo.fieldAccessor)
                {
                    case CORINFO_FIELD_INSTANCE:
#ifdef FEATURE_READYTORUN
                    case CORINFO_FIELD_INSTANCE_WITH_BASE:
#endif
                    {
                        op1 = gtNewFieldAddrNode(resolvedToken.hField, obj, fieldInfo.offset);

#ifdef FEATURE_READYTORUN
                        if (fieldInfo.fieldAccessor == CORINFO_FIELD_INSTANCE_WITH_BASE)
                        {
                            op1->AsFieldAddr()->gtFieldLookup = fieldInfo.fieldLookup;
                        }
#endif
                        if (StructHasOverlappingFields(info.compCompHnd->getClassAttribs(resolvedToken.hClass)))
                        {
                            op1->AsFieldAddr()->gtFldMayOverlap = true;
                        }

                        if (compIsForInlining() &&
                            impInlineIsGuaranteedThisDerefBeforeAnySideEffects(op2, nullptr, obj,
                                                                               impInlineInfo->inlArgInfo))
                        {
                            impInlineInfo->thisDereferencedFirst = true;
                        }
                    }
                    break;
                    case CORINFO_FIELD_STATIC_TLS:
#ifdef TARGET_X86
                        // Legacy TLS access is implemented as intrinsic on x86 only.
                        op1 = gtNewFieldAddrNode(TYP_I_IMPL, resolvedToken.hField, nullptr, fieldInfo.offset);
                        op1->gtFlags |= GTF_FLD_TLS; // fgMorphExpandTlsField will handle the transformation.
                        break;
#else
                        fieldInfo.fieldAccessor = CORINFO_FIELD_STATIC_ADDR_HELPER;
                        FALLTHROUGH;
#endif
                    case CORINFO_FIELD_STATIC_ADDR_HELPER:
                    case CORINFO_FIELD_INSTANCE_HELPER:
                    case CORINFO_FIELD_INSTANCE_ADDR_HELPER:
                        op1 = gtNewRefCOMfield(obj, &resolvedToken, (CORINFO_ACCESS_FLAGS)aflags, &fieldInfo, lclTyp,
                                               op2);
                        goto SPILL_APPEND;

                    case CORINFO_FIELD_STATIC_TLS_MANAGED:
                        setMethodHasTlsFieldAccess();
                        FALLTHROUGH;
                    case CORINFO_FIELD_STATIC_ADDRESS:
                    case CORINFO_FIELD_STATIC_RVA_ADDRESS:
                    case CORINFO_FIELD_STATIC_SHARED_STATIC_HELPER:
                    case CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER:
                    case CORINFO_FIELD_STATIC_READYTORUN_HELPER:
                    case CORINFO_FIELD_STATIC_RELOCATABLE:
                        // Handled above
                        break;

                    default:
                        assert(!"Unexpected fieldAccessor");
                }

                    /* V4.0 allows stores of i4 constant values to i8 type vars when IL verifier is bypassed (full
                    trust apps). The reason this works is that JIT stores an i4 constant in GenTree union during
                    importation and reads from the union as if it were a long during code generation. Though this
                    can potentially read garbage, one can get lucky to have this working correctly.

                    This code pattern is generated by Dev10 MC++ compiler while storing to fields when compiled with
                    /O2 switch (default when compiling retail configs in Dev10) and a customer app has taken a
                    dependency on it. To be backward compatible, we will explicitly add an upward cast here so that
                    it works correctly always.

                    Note that this is limited to x86 alone as there is no back compat to be addressed for Arm JIT
                    for V4.0.
                    */

#ifndef TARGET_64BIT
                // In UWP6.0 and beyond (post-.NET Core 2.0), we decided to let this cast from int to long be
                // generated for ARM as well as x86, so the following IR will be accepted:
                // STMTx (IL 0x... ???)
                //   *  STORE_LCL_VAR long
                //   \--*  CNS_INT   int    2

                if ((lclTyp != op2->TypeGet()) && op2->OperIsConst() && varTypeIsIntOrI(op2->TypeGet()) &&
                    (lclTyp == TYP_LONG))
                {
                    op2 = gtNewCastNode(lclTyp, op2, false, lclTyp);
                }
#endif
                // Allow a downcast of op2 from TYP_I_IMPL into a 32-bit Int for x86 JIT compatibility.
                // Allow an upcast of op2 from a 32-bit Int into TYP_I_IMPL for x86 JIT compatibility.
                op2 = impImplicitIorI4Cast(op2, lclTyp);
                op2 = impImplicitR4orR8Cast(op2, lclTyp);

                // Currently, *all* TYP_REF statics are stored inside an "object[]" array that itself
                // resides on the managed heap, and so we can use an unchecked write barrier for this
                // store. Likewise if we're storing to a field of an on-heap object.
                if ((lclTyp == TYP_REF) &&
                    (((fieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) != 0) || obj->TypeIs(TYP_REF)))
                {
                    indirFlags |= GTF_IND_TGT_HEAP;
                }
                else if ((lclTyp == TYP_STRUCT) && (fieldInfo.structType != NO_CLASS_HANDLE) &&
                         eeIsByrefLike(fieldInfo.structType))
                {
                    // Field's type is a byref-like struct -> address is not on the heap.
                    indirFlags |= GTF_IND_TGT_NOT_HEAP;
                }
                else
                {
                    // Field's owner is a byref-like struct -> address is not on the heap.
                    CORINFO_CLASS_HANDLE fldOwner = info.compCompHnd->getFieldClass(resolvedToken.hField);
                    if ((fldOwner != NO_CLASS_HANDLE) && eeIsByrefLike(fldOwner))
                    {
                        indirFlags |= GTF_IND_TGT_NOT_HEAP;
                    }
                }

                assert(varTypeIsI(op1));
                op1 = (lclTyp == TYP_STRUCT) ? gtNewStoreBlkNode(layout, op1, op2, indirFlags)->AsIndir()
                                             : gtNewStoreIndNode(lclTyp, op1, op2, indirFlags);
                impAnnotateFieldIndir(op1->AsIndir());

                if (varTypeIsStruct(op1))
                {
                    op1 = impStoreStruct(op1, CHECK_SPILL_ALL);
                }
                goto SPILL_APPEND;
            }

            case CEE_NEWARR:
            {

                /* Get the class type index operand */

                _impResolveToken(CORINFO_TOKENKIND_Newarr);

                JITDUMP(" %08X", resolvedToken.token);

                if (!IsAot())
                {
                    // Need to restore array classes before creating array objects on the heap
                    op1 = impTokenToHandle(&resolvedToken, nullptr, true /*mustRestoreHandle*/);
                    if (op1 == nullptr)
                    { // compDonotInline()
                        return;
                    }
                }

                tiRetVal = makeTypeInfo(resolvedToken.hClass);

                accessAllowedResult =
                    info.compCompHnd->canAccessClass(&resolvedToken, info.compMethodHnd, &calloutHelper);
                impHandleAccessAllowed(accessAllowedResult, &calloutHelper);

                /* Form the arglist: array class handle, size */
                op2 = impPopStack().val;
                assertImp(genActualTypeIsIntOrI(op2));

                // The array helper takes a native int for array length.
                // So if we have an int, explicitly extend it to be a native int.
                op2 = impImplicitIorI4Cast(op2, TYP_I_IMPL);

                bool isFrozenAllocator = false;
                // If we're jitting a static constructor and detect the following code pattern:
                //
                //  newarr
                //  stsfld
                //  ret
                //
                // we emit a "frozen" allocator for newarr to, hopefully, allocate that array on a frozen segment.
                // This is a very simple and conservative implementation targeting Array.Empty<T>()'s shape
                // Ideally, we want to be able to use frozen allocators more broadly, but such an analysis is
                // not trivial.
                //
                if (((info.compFlags & FLG_CCTOR) == FLG_CCTOR) &&
                    // Does VM allow us to use frozen allocators?
                    opts.jitFlags->IsSet(JitFlags::JIT_FLAG_FROZEN_ALLOC_ALLOWED))
                {
                    // Check next two opcodes (have to be STSFLD and RET)
                    const BYTE* nextOpcode1 = codeAddr + sizeof(mdToken);
                    const BYTE* nextOpcode2 = nextOpcode1 + sizeof(mdToken) + 1;
                    if ((nextOpcode2 < codeEndp) && (getU1LittleEndian(nextOpcode1) == CEE_STSFLD))
                    {
                        if (getU1LittleEndian(nextOpcode2) == CEE_RET)
                        {
                            // Check that the field is "static readonly", we don't want to waste memory
                            // for potentially mutable fields.
                            CORINFO_RESOLVED_TOKEN fldToken;
                            impResolveToken(nextOpcode1 + 1, &fldToken, CORINFO_TOKENKIND_Field);
                            CORINFO_FIELD_INFO fi;
                            eeGetFieldInfo(&fldToken, CORINFO_ACCESS_SET, &fi);
                            unsigned flagsToCheck = CORINFO_FLG_FIELD_STATIC | CORINFO_FLG_FIELD_FINAL;
                            if (((fi.fieldFlags & flagsToCheck) == flagsToCheck) && !eeIsSharedInst(info.compClassHnd))
                            {
#ifdef FEATURE_READYTORUN
                                if (IsAot())
                                {
                                    // Need to restore array classes before creating array objects on the heap
                                    op1 = impTokenToHandle(&resolvedToken, nullptr, true /*mustRestoreHandle*/);
                                }
#endif
                                op1 = gtNewHelperCallNode(CORINFO_HELP_NEWARR_1_MAYBEFROZEN, TYP_REF, op1, op2);
                                isFrozenAllocator = true;
                            }
                        }
                    }
                }

#ifdef FEATURE_READYTORUN
                if (IsAot() && !isFrozenAllocator)
                {
                    helper                = CORINFO_HELP_READYTORUN_NEWARR_1;
                    op1                   = impReadyToRunHelperToTree(&resolvedToken, helper, TYP_REF, nullptr, op2);
                    usingReadyToRunHelper = (op1 != nullptr);

                    if (!usingReadyToRunHelper)
                    {
                        // TODO: ReadyToRun: When generic dictionary lookups are necessary, replace the lookup call
                        // and the newarr call with a single call to a dynamic R2R cell that will:
                        //      1) Load the context
                        //      2) Perform the generic dictionary lookup and caching, and generate the appropriate stub
                        //      3) Allocate the new array
                        // Reason: performance (today, we'll always use the slow helper for the R2R generics case)

                        op1 = impTokenToHandle(&resolvedToken, nullptr, true /*mustRestoreHandle*/);
                        if (op1 == nullptr)
                        { // compDonotInline()
                            return;
                        }
                    }
                }

                if (!usingReadyToRunHelper && !isFrozenAllocator)
#endif
                {
                    /* Create a call to 'new' */
                    helper = info.compCompHnd->getNewArrHelper(resolvedToken.hClass);

                    // Note that this only works for shared generic code because the same helper is used for all
                    // reference array types
                    op1 = gtNewHelperCallNode(helper, TYP_REF, op1, op2);
                }

                op1->AsCall()->compileTimeHelperArgumentHandle = (CORINFO_GENERIC_HANDLE)resolvedToken.hClass;

                // Remember that this function contains 'new' of an SD array.
                optMethodFlags |= OMF_HAS_NEWARRAY;
                block->SetFlags(BBF_HAS_NEWARR);

                if (opts.OptimizationEnabled())
                {
                    // We assign the newly allocated object (by a GT_CALL to newarr node)
                    // to a temp. Note that the pattern "temp = allocArr" is required
                    // by ObjectAllocator phase to be able to determine newarr nodes
                    // without exhaustive walk over all expressions.
                    lclNum = lvaGrabTemp(true DEBUGARG("NewArr temp"));

                    impStoreToTemp(lclNum, op1, CHECK_SPILL_ALL);

                    assert(lvaTable[lclNum].lvSingleDef == 0);
                    lvaTable[lclNum].lvSingleDef = 1;
                    JITDUMP("Marked V%02u as a single def local\n", lclNum);
                    lvaSetClass(lclNum, resolvedToken.hClass, true /* is Exact */);

                    /* Push the result of the call on the stack */

                    impPushOnStack(gtNewLclvNode(lclNum, TYP_REF), tiRetVal);

#ifdef DEBUG
                    // Under SPMI, look up info we might ask for if we stack allocate this array,
                    // but only if we know the precise type
                    //
                    if (JitConfig.EnableExtraSuperPmiQueries() && !eeIsSharedInst(resolvedToken.hClass))
                    {
                        void* pEmbedClsHnd;
                        info.compCompHnd->embedClassHandle(resolvedToken.hClass, &pEmbedClsHnd);
                        CORINFO_CLASS_HANDLE elemClsHnd = NO_CLASS_HANDLE;
                        CorInfoType elemCorType = info.compCompHnd->getChildType(resolvedToken.hClass, &elemClsHnd);
                        var_types   elemType    = JITtype2varType(elemCorType);
                        if (elemType == TYP_STRUCT)
                        {
                            typGetObjLayout(elemClsHnd);
                            info.compCompHnd->isValueClass(elemClsHnd);
                        }
                        compGetHelperFtn(CORINFO_HELP_MEMZERO);
                    }
#endif
                }
                else
                {
                    /* Push the result of the call on the stack */
                    impPushOnStack(op1, tiRetVal);
                }

                callTyp = TYP_REF;
            }
            break;

            case CEE_LOCALLOC:
                // We don't allow locallocs inside handlers
                if (block->hasHndIndex())
                {
                    BADCODE("Localloc can't be inside handler");
                }

                // Get the size to allocate

                op2 = impPopStack().val;
                assertImp(genActualTypeIsIntOrI(op2->gtType));

                if (stackState.esStackDepth != 0)
                {
                    BADCODE("Localloc can only be used when the stack is empty");
                }

                // If the localloc is not in a loop and its size is a small constant,
                // create a new block layout struct local var and return its address.
                {
                    bool convertedToLocal = false;

                    // Need to aggressively fold here, as even fixed-size locallocs
                    // will have casts in the way.
                    op2 = gtFoldExpr(op2);

                    if (op2->IsIntegralConst())
                    {
                        const ssize_t allocSize = op2->AsIntCon()->IconValue();

                        bool bbInALoop = impBlockIsInALoop(block);

                        if (allocSize == 0)
                        {
                            // Result is nullptr
                            JITDUMP("Converting stackalloc of 0 bytes to push null unmanaged pointer\n");
                            op1              = gtNewIconNode(0, TYP_I_IMPL);
                            convertedToLocal = true;
                        }
                        else if ((allocSize > 0) && !bbInALoop)
                        {
                            // Get the size threshold for local conversion
                            ssize_t maxSize = DEFAULT_MAX_LOCALLOC_TO_LOCAL_SIZE;

#ifdef DEBUG
                            // Optionally allow this to be modified
                            maxSize = JitConfig.JitStackAllocToLocalSize();
#endif // DEBUG

                            if (allocSize <= maxSize)
                            {
                                const unsigned stackallocAsLocal = lvaGrabTemp(false DEBUGARG("stackallocLocal"));
                                JITDUMP("Converting stackalloc of %zd bytes to new local V%02u\n", allocSize,
                                        stackallocAsLocal);
                                lvaSetStruct(stackallocAsLocal, typGetBlkLayout((unsigned)allocSize), false);
                                lvaTable[stackallocAsLocal].lvHasLdAddrOp    = true;
                                lvaTable[stackallocAsLocal].lvIsUnsafeBuffer = true;
                                op1                                          = gtNewLclVarAddrNode(stackallocAsLocal);
                                convertedToLocal                             = true;

                                if (compIsForInlining() && info.compInitMem && !impInlineRoot()->info.compInitMem)
                                {
                                    // Explicitly zero out the local if we're inlining a method with InitLocals into a
                                    // method without InitLocals.
                                    impStoreToTemp(stackallocAsLocal, gtNewIconNode(0), CHECK_SPILL_ALL);
                                }

                                if (!this->opts.compDbgEnC)
                                {
                                    // Ensure we have stack security for this method.
                                    // Reorder layout since the converted localloc is treated as an unsafe buffer.
                                    setNeedsGSSecurityCookie();
                                    compGSReorderStackLayout = true;
                                }
                            }
                        }
                    }

                    if (!convertedToLocal)
                    {
                        // Bail out if inlining and the localloc was not converted.
                        //
                        // Note we might consider allowing the inline, if the call
                        // site is not in a loop.
                        if (compIsForInlining())
                        {
                            InlineObservation obs = op2->IsIntegralConst()
                                                        ? InlineObservation::CALLEE_LOCALLOC_TOO_LARGE
                                                        : InlineObservation::CALLSITE_LOCALLOC_SIZE_UNKNOWN;
                            compInlineResult->NoteFatal(obs);
                            return;
                        }

                        op1 = gtNewOperNode(GT_LCLHEAP, TYP_I_IMPL, op2);
                        // May throw a stack overflow exception. Obviously, we don't want locallocs to be CSE'd.
                        op1->gtFlags |= (GTF_EXCEPT | GTF_DONT_CSE);

                        // Ensure we have stack security for this method.
                        setNeedsGSSecurityCookie();

                        /* The FP register may not be back to the original value at the end
                           of the method, even if the frame size is 0, as localloc may
                           have modified it. So we will HAVE to reset it */
                        compLocallocUsed = true;
                    }
                    else
                    {
                        compLocallocOptimized = true;
                    }
                }

                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_ISINST:
            {
                /* Get the type token */
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Casting);

                JITDUMP(" %08X", resolvedToken.token);

                if (!IsAot())
                {
                    op2 = impTokenToHandle(&resolvedToken, nullptr, false);
                    if (op2 == nullptr)
                    { // compDonotInline()
                        return;
                    }
                }

                accessAllowedResult =
                    info.compCompHnd->canAccessClass(&resolvedToken, info.compMethodHnd, &calloutHelper);
                impHandleAccessAllowed(accessAllowedResult, &calloutHelper);

                op1 = impPopStack().val;

                GenTree* optTree = impOptimizeCastClassOrIsInst(op1, &resolvedToken, false);

                if (optTree != nullptr)
                {
                    impPushOnStack(optTree, tiRetVal);
                }
                else
                {

#ifdef FEATURE_READYTORUN
                    if (IsAot())
                    {
                        GenTreeCall* opLookup =
                            impReadyToRunHelperToTree(&resolvedToken, CORINFO_HELP_READYTORUN_ISINSTANCEOF, TYP_REF,
                                                      nullptr, op1);
                        usingReadyToRunHelper = (opLookup != nullptr);
                        op1                   = (usingReadyToRunHelper ? opLookup : op1);

                        if (!usingReadyToRunHelper)
                        {
                            // TODO: ReadyToRun: When generic dictionary lookups are necessary, replace the lookup call
                            // and the isinstanceof_any call with a single call to a dynamic R2R cell that will:
                            //      1) Load the context
                            //      2) Perform the generic dictionary lookup and caching, and generate the appropriate
                            //      stub
                            //      3) Perform the 'is instance' check on the input object
                            // Reason: performance (today, we'll always use the slow helper for the R2R generics case)

                            op2 = impTokenToHandle(&resolvedToken, nullptr, false);
                            if (op2 == nullptr)
                            { // compDonotInline()
                                return;
                            }
                        }
                    }

                    if (!usingReadyToRunHelper)
#endif
                    {
                        int  consumed     = 0;
                        bool booleanCheck = impMatchIsInstBooleanConversion(codeAddr + sz, codeEndp, &consumed);
                        op1 = impCastClassOrIsInstToTree(op1, op2, &resolvedToken, false, &booleanCheck, opcodeOffs);

                        if (booleanCheck)
                        {
                            sz += consumed;
                        }
                    }
                    if (compDonotInline())
                    {
                        return;
                    }

                    impPushOnStack(op1, tiRetVal);
                }
                break;
            }

            case CEE_REFANYVAL:
            {

                // get the class handle and make a ICON node out of it

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                op2 = impTokenToHandle(&resolvedToken);
                if (op2 == nullptr)
                { // compDonotInline()
                    return;
                }

                op1 = impPopStack().val;
                // make certain it is normalized;
                op1 = impNormStructVal(op1, CHECK_SPILL_ALL);

                // Call helper GETREFANY(classHandle, op1);
                GenTreeCall* helperCall   = gtNewHelperCallNode(CORINFO_HELP_GETREFANY, TYP_BYREF);
                NewCallArg   clsHandleArg = NewCallArg::Primitive(op2);
                NewCallArg   typedRefArg  = NewCallArg::Struct(op1, TYP_STRUCT, typGetObjLayout(impGetRefAnyClass()));
                helperCall->gtArgs.PushFront(this, clsHandleArg, typedRefArg);
                helperCall->gtFlags |= (op1->gtFlags | op2->gtFlags) & GTF_ALL_EFFECT;
                op1 = helperCall;

                impPushOnStack(op1, tiRetVal);
                break;
            }
            case CEE_REFANYTYPE:
            {
                op1 = impPopStack().val;

                // Get the address of the refany
                GenTreeFlags indirFlags = GTF_EMPTY;
                op1                     = impGetNodeAddr(op1, CHECK_SPILL_ALL, &indirFlags);

                // Fetch the type from the correct slot
                op1 = gtNewOperNode(GT_ADD, TYP_BYREF, op1,
                                    gtNewIconNode(OFFSETOF__CORINFO_TypedReference__type, TYP_I_IMPL));
                op1 = gtNewIndir(TYP_BYREF, op1, indirFlags);

                // Convert native TypeHandle to RuntimeTypeHandle.
                op1 = gtNewHelperCallNode(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE_MAYBENULL, TYP_STRUCT, op1);

                CORINFO_CLASS_HANDLE classHandle = impGetTypeHandleClass();

                // The handle struct is returned in register
                op1->AsCall()->gtReturnType = GetRuntimeHandleUnderlyingType();
                op1->AsCall()->gtRetClsHnd  = classHandle;
#if FEATURE_MULTIREG_RET
                op1->AsCall()->InitializeStructReturnType(this, classHandle, op1->AsCall()->GetUnmanagedCallConv());
#endif

                tiRetVal = typeInfo(TYP_STRUCT);
                impPushOnStack(op1, tiRetVal);
            }
            break;

            case CEE_LDTOKEN:
            {
                /* Get the Class index */
                assertImp(sz == sizeof(unsigned));
                lastLoadToken = codeAddr;
                _impResolveToken(CORINFO_TOKENKIND_Ldtoken);

                tokenType = info.compCompHnd->getTokenTypeAsHandle(&resolvedToken);

                op1 = impTokenToHandle(&resolvedToken, nullptr, true);
                if (op1 == nullptr)
                { // compDonotInline()
                    return;
                }

                helper = CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE;
                assert(resolvedToken.hClass != nullptr);

                if (resolvedToken.hMethod != nullptr)
                {
                    helper = CORINFO_HELP_METHODDESC_TO_STUBRUNTIMEMETHOD;
                }
                else if (resolvedToken.hField != nullptr)
                {
                    helper = CORINFO_HELP_FIELDDESC_TO_STUBRUNTIMEFIELD;
                }

                op1 = gtNewHelperCallNode(helper, TYP_STRUCT, op1);

                // The handle struct is returned in register and
                // it could be consumed both as `TYP_STRUCT` and `TYP_REF`.
                op1->AsCall()->gtReturnType = GetRuntimeHandleUnderlyingType();
#if FEATURE_MULTIREG_RET
                op1->AsCall()->InitializeStructReturnType(this, tokenType, op1->AsCall()->GetUnmanagedCallConv());
#endif
                op1->AsCall()->gtRetClsHnd = tokenType;

                tiRetVal = makeTypeInfo(tokenType);
                impPushOnStack(op1, tiRetVal);
            }
            break;

            case CEE_UNBOX:
            case CEE_UNBOX_ANY:
            {
                /* Get the Class index */
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                bool runtimeLookup;
                op2 = impTokenToHandle(&resolvedToken, &runtimeLookup);
                if (op2 == nullptr)
                {
                    assert(compDonotInline());
                    return;
                }

                // Run this always so we can get access exceptions even with SkipVerification.
                accessAllowedResult =
                    info.compCompHnd->canAccessClass(&resolvedToken, info.compMethodHnd, &calloutHelper);
                impHandleAccessAllowed(accessAllowedResult, &calloutHelper);

                if (opcode == CEE_UNBOX_ANY && !eeIsValueClass(resolvedToken.hClass))
                {
                    JITDUMP("\n Importing UNBOX.ANY(refClass) as CASTCLASS\n");
                    op1 = impPopStack().val;
                    goto CASTCLASS;
                }

                /* Pop the object and create the unbox helper call */
                /* You might think that for UNBOX_ANY we need to push a different */
                /* (non-byref) type, but here we're making the tiRetVal that is used */
                /* for the intermediate pointer which we then transfer onto the BLK */
                /* instruction. BLK then creates the appropriate tiRetVal. */

                op1 = impPopStack().val;
                assertImp(op1->TypeIs(TYP_REF));

                helper = info.compCompHnd->getUnBoxHelper(resolvedToken.hClass);
                assert(helper == CORINFO_HELP_UNBOX || helper == CORINFO_HELP_UNBOX_NULLABLE);

                // Check legality and profitability of inline expansion for unboxing.
                const bool canExpandInline    = (helper == CORINFO_HELP_UNBOX);
                const bool shouldExpandInline = !compCurBB->isRunRarely() && opts.OptimizationEnabled();

                if (canExpandInline && shouldExpandInline)
                {
                    // See if we know anything about the type of op1, the object being unboxed.
                    bool                 isExact   = false;
                    bool                 isNonNull = false;
                    CORINFO_CLASS_HANDLE clsHnd    = gtGetClassHandle(op1, &isExact, &isNonNull);

                    // We can skip the "exact" bit here as we are comparing to a value class.
                    // compareTypesForEquality should bail on comparisons for shared value classes.
                    if (clsHnd != NO_CLASS_HANDLE)
                    {
                        const TypeCompareState compare =
                            info.compCompHnd->compareTypesForEquality(resolvedToken.hClass, clsHnd);

                        if (compare == TypeCompareState::Must)
                        {
                            JITDUMP("\nOptimizing %s (%s) -- type test will succeed\n",
                                    opcode == CEE_UNBOX ? "UNBOX" : "UNBOX.ANY", eeGetClassName(clsHnd));

                            // For UNBOX, null check (if necessary), and then leave the box payload byref on the stack.
                            if (opcode == CEE_UNBOX)
                            {
                                GenTree* cloneOperand;
                                op1 = impCloneExpr(op1, &cloneOperand, CHECK_SPILL_ALL,
                                                   nullptr DEBUGARG("optimized unbox clone"));

                                GenTree* boxPayloadOffset = gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL);
                                GenTree* boxPayloadAddress =
                                    gtNewOperNode(GT_ADD, TYP_BYREF, cloneOperand, boxPayloadOffset);
                                GenTree* nullcheck = gtNewNullCheck(op1);
                                // Add an ordering dependency between the null
                                // check and forming the byref; the JIT assumes
                                // in many places that the only legal null
                                // byref is literally 0, and since the byref
                                // leaks out here, we need to ensure it is
                                // nullchecked.
                                nullcheck->SetHasOrderingSideEffect();
                                boxPayloadAddress->SetHasOrderingSideEffect();
                                GenTree* result = gtNewOperNode(GT_COMMA, TYP_BYREF, nullcheck, boxPayloadAddress);
                                impPushOnStack(result, tiRetVal);
                                break;
                            }

                            // For UNBOX.ANY load the struct from the box payload byref (the load will nullcheck)
                            assert(opcode == CEE_UNBOX_ANY);
                            GenTree* boxPayloadOffset  = gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL);
                            GenTree* boxPayloadAddress = gtNewOperNode(GT_ADD, TYP_BYREF, op1, boxPayloadOffset);
                            impPushOnStack(boxPayloadAddress, tiRetVal);
                            goto OBJ;
                        }
                        else
                        {
                            JITDUMP("\nUnable to optimize %s -- can't resolve type comparison\n",
                                    opcode == CEE_UNBOX ? "UNBOX" : "UNBOX.ANY");
                        }
                    }
                    else
                    {
                        JITDUMP("\nUnable to optimize %s -- class for [%06u] not known\n",
                                opcode == CEE_UNBOX ? "UNBOX" : "UNBOX.ANY", dspTreeID(op1));
                    }

                    JITDUMP("\n Importing %s as inline sequence\n", opcode == CEE_UNBOX ? "UNBOX" : "UNBOX.ANY");
                    // we are doing normal unboxing
                    // inline the common case of the unbox helper
                    // UNBOX(exp) morphs into
                    // clone = pop(exp);
                    // ((*clone == typeToken) ? nop : helper(clone, typeToken));
                    // push(clone + TARGET_POINTER_SIZE)
                    //
                    GenTree* cloneOperand;
                    op1 = impCloneExpr(op1, &cloneOperand, CHECK_SPILL_ALL, nullptr DEBUGARG("inline UNBOX clone1"));
                    op1 = gtNewMethodTableLookup(op1);

                    GenTree* condBox = gtNewOperNode(GT_EQ, TYP_INT, op1, op2);

                    op1 = impCloneExpr(cloneOperand, &cloneOperand, CHECK_SPILL_ALL,
                                       nullptr DEBUGARG("inline UNBOX clone2"));
                    op2 = impTokenToHandle(&resolvedToken);
                    if (op2 == nullptr)
                    { // compDonotInline()
                        return;
                    }
                    op1 = gtNewHelperCallNode(helper, TYP_VOID, op2, op1);

                    op1 = new (this, GT_COLON) GenTreeColon(TYP_VOID, gtNewNothingNode(), op1);
                    op1 = gtNewQmarkNode(TYP_VOID, condBox, op1->AsColon());

                    // QMARK nodes cannot reside on the evaluation stack. Because there
                    // may be other trees on the evaluation stack that side-effect the
                    // sources of the UNBOX operation we must spill the stack.

                    impAppendTree(op1, CHECK_SPILL_ALL, impCurStmtDI);

                    // Create the address-expression to reference past the object header
                    // to the beginning of the value-type. Today this means adjusting
                    // past the base of the objects vtable field which is pointer sized.

                    op2 = gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL);
                    op1 = gtNewOperNode(GT_ADD, TYP_BYREF, cloneOperand, op2);
                }
                else if (helper == CORINFO_HELP_UNBOX_NULLABLE)
                {
                    // op1 is the object being unboxed
                    // op2 is either a class handle node or a runtime lookup node (it's fine to reorder)
                    op1 = impInlineUnboxNullable(resolvedToken.hClass, op2, op1);
                }
                else
                {
                    // Don't optimize, just call the helper and be done with it
                    JITDUMP("\n Importing %s as helper call because %s\n", opcode == CEE_UNBOX ? "UNBOX" : "UNBOX.ANY",
                            canExpandInline ? "want smaller code or faster jitting" : "inline expansion not legal");

                    assert(helper == CORINFO_HELP_UNBOX);
                    op1 = gtNewHelperCallNode(helper, TYP_BYREF, op2, op1);
                }

                assert((helper == CORINFO_HELP_UNBOX && op1->TypeIs(TYP_BYREF)) || // Unbox helper returns a byref.
                       (helper == CORINFO_HELP_UNBOX_NULLABLE && op1->TypeIs(TYP_STRUCT)) // UnboxNullable helper
                                                                                          // returns a struct.
                );

                /*
                  ----------------------------------------------------------------------
                  | \ helper  |                         |                              |
                  |   \       |                         |                              |
                  |     \     | CORINFO_HELP_UNBOX      | CORINFO_HELP_UNBOX_NULLABLE  |
                  |       \   | (which returns a BYREF) | (which returns a STRUCT)     |                              |
                  | opcode  \ |                         |                              |
                  |---------------------------------------------------------------------
                  | UNBOX     | push the BYREF          | spill the STRUCT to a local, |
                  |           |                         | push the BYREF to this local |
                  |---------------------------------------------------------------------
                  | UNBOX_ANY | push a GT_BLK of        | push the STRUCT local        |
                  |           | the BYREF               |                              |
                  |---------------------------------------------------------------------
                */

                if (opcode == CEE_UNBOX)
                {
                    if (helper == CORINFO_HELP_UNBOX_NULLABLE)
                    {
                        // NOTE: what we do here doesn't comply with the ECMA spec, see
                        // https://github.com/dotnet/runtime/issues/86203#issuecomment-1546709542
                        // Although, now with escape analysis being enabled we can afford a temp GC alloc here?

                        // Unbox nullable helper returns a struct type.
                        // We need to spill it to a temp so than can take the address of it.
                        // Here we need unsafe value cls check, since the address of struct is taken to be used
                        // further along and potetially be exploitable.

                        // op1 is always a local, see code above for CORINFO_HELP_UNBOX_NULLABLE
                        op1 = gtNewLclVarAddrNode(op1->AsLclVar()->GetLclNum(), TYP_I_IMPL);
                    }
                }
                else
                {
                    assert(opcode == CEE_UNBOX_ANY);

                    if (helper == CORINFO_HELP_UNBOX)
                    {
                        // Normal unbox helper returns a TYP_BYREF.
                        impPushOnStack(op1, tiRetVal);
                        goto OBJ;
                    }

                    assert(helper == CORINFO_HELP_UNBOX_NULLABLE && "Make sure the helper is nullable!");

                    // If non register passable struct we have it materialized in the RetBuf.
                    assert(op1->TypeIs(TYP_STRUCT));
                    tiRetVal = makeTypeInfo(resolvedToken.hClass);
                }

                impPushOnStack(op1, tiRetVal);
            }
            break;

            case CEE_BOX:
            {
                /* Get the Class index */
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Box);

                JITDUMP(" %08X", resolvedToken.token);

                accessAllowedResult =
                    info.compCompHnd->canAccessClass(&resolvedToken, info.compMethodHnd, &calloutHelper);
                impHandleAccessAllowed(accessAllowedResult, &calloutHelper);

                // Note BOX can be used on things that are not value classes, in which
                // case we get a NOP.  However the verifier's view of the type on the
                // stack changes (in generic code a 'T' becomes a 'boxed T')
                if (!eeIsValueClass(resolvedToken.hClass))
                {
                    JITDUMP("\n Importing BOX(refClass) as NOP\n");
                    stackState.esStack[stackState.esStackDepth - 1].seTypeInfo = tiRetVal;
                    break;
                }

                bool isByRefLike = eeIsByrefLike(resolvedToken.hClass);
                if (isByRefLike)
                {
                    // For ByRefLike types we are required to either fold the
                    // recognized patterns in impBoxPatternMatch or otherwise
                    // throw InvalidProgramException at runtime. In either case
                    // we will need to spill side effects of the expression.
                    impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("Required for box of ByRefLike type"));
                }

                // Look ahead for box idioms
                int matched = impBoxPatternMatch(&resolvedToken, codeAddr + sz, codeEndp,
                                                 isByRefLike ? BoxPatterns::IsByRefLike : BoxPatterns::None);
                if (matched >= 0)
                {
                    // Skip the matched IL instructions
                    sz += matched;
                    break;
                }

                if (isByRefLike)
                {
                    // ByRefLike types are supported in boxing scenarios when the instruction can be elided
                    // due to a recognized pattern above. If the pattern is not recognized, the code is invalid.
                    BADCODE("ByRefLike types cannot be boxed");
                }
                else
                {
                    impImportAndPushBox(&resolvedToken);
                    if (compDonotInline())
                    {
                        return;
                    }
                }
            }
            break;

            case CEE_SIZEOF:

                /* Get the Class index */
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                op1 = gtNewIconNode(info.compCompHnd->getClassSize(resolvedToken.hClass));
                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_CASTCLASS:

                /* Get the Class index */

                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Casting);

                JITDUMP(" %08X", resolvedToken.token);

                if (!IsAot())
                {
                    op2 = impTokenToHandle(&resolvedToken, nullptr, false);
                    if (op2 == nullptr)
                    { // compDonotInline()
                        return;
                    }
                }

                accessAllowedResult =
                    info.compCompHnd->canAccessClass(&resolvedToken, info.compMethodHnd, &calloutHelper);
                impHandleAccessAllowed(accessAllowedResult, &calloutHelper);

                op1 = impPopStack().val;

            /* Pop the address and create the 'checked cast' helper call */

            // At this point we expect typeRef to contain the token, op1 to contain the value being cast,
            // and op2 to contain code that creates the type handle corresponding to typeRef
            CASTCLASS:
            {
                GenTree* optTree = impOptimizeCastClassOrIsInst(op1, &resolvedToken, true);

                if (optTree != nullptr)
                {
                    impPushOnStack(optTree, tiRetVal);
                }
                else
                {

#ifdef FEATURE_READYTORUN
                    if (IsAot())
                    {
                        GenTreeCall* opLookup =
                            impReadyToRunHelperToTree(&resolvedToken, CORINFO_HELP_READYTORUN_CHKCAST, TYP_REF, nullptr,
                                                      op1);
                        usingReadyToRunHelper = (opLookup != nullptr);
                        op1                   = (usingReadyToRunHelper ? opLookup : op1);

                        if (!usingReadyToRunHelper)
                        {
                            // TODO: ReadyToRun: When generic dictionary lookups are necessary, replace the lookup call
                            // and the chkcastany call with a single call to a dynamic R2R cell that will:
                            //      1) Load the context
                            //      2) Perform the generic dictionary lookup and caching, and generate the appropriate
                            //      stub
                            //      3) Check the object on the stack for the type-cast
                            // Reason: performance (today, we'll always use the slow helper for the R2R generics case)

                            op2 = impTokenToHandle(&resolvedToken, nullptr, false);
                            if (op2 == nullptr)
                            { // compDonotInline()
                                return;
                            }
                        }
                    }

                    if (!usingReadyToRunHelper)
#endif
                    {
                        bool booleanCheck = false;
                        op1 = impCastClassOrIsInstToTree(op1, op2, &resolvedToken, true, &booleanCheck, opcodeOffs);
                    }
                    if (compDonotInline())
                    {
                        return;
                    }

                    /* Push the result back on the stack */
                    impPushOnStack(op1, tiRetVal);
                }
            }
            break;

            case CEE_THROW:

                if (!fgPgoSynthesized)
                {
                    // Any block with a throw is rarely executed.
                    block->bbSetRunRarely();
                }

                // Pop the exception object and create the 'throw' helper call
                op1 = gtNewHelperCallNode(CORINFO_HELP_THROW, TYP_VOID, impPopStack().val);

                // Fall through to clear out the eval stack.

            EVAL_APPEND:
                if (stackState.esStackDepth > 0)
                {
                    impEvalSideEffects();
                }

                assert(stackState.esStackDepth == 0);

                goto APPEND;

            case CEE_RETHROW:

                assert(!compIsForInlining());

                if (info.compXcptnsCount == 0)
                {
                    BADCODE("rethrow outside catch");
                }

                /* Create the 'rethrow' helper call */

                op1 = gtNewHelperCallNode(CORINFO_HELP_RETHROW, TYP_VOID);

                goto EVAL_APPEND;

            case CEE_INITOBJ:
            {
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                ClassLayout* layout;
                lclTyp = TypeHandleToVarType(resolvedToken.hClass, &layout);

                if (lclTyp != TYP_STRUCT)
                {
                    op2 = gtNewZeroConNode(lclTyp);
                    goto STIND_VALUE;
                }

                op1 = impPopStack().val;
                op2 = gtNewIconNode(0);
                op1 = gtNewStoreValueNode(layout, op1, op2);
                goto SPILL_APPEND;
            }

            case CEE_INITBLK:
            case CEE_CPBLK:
            {
                GenTreeFlags indirFlags = impPrefixFlagsToIndirFlags(prefixFlags);
                const bool   isVolatile = (indirFlags & GTF_IND_VOLATILE) != 0;
#ifndef TARGET_X86
                if (isVolatile && !impStackTop(0).val->IsCnsIntOrI())
                {
                    // We're going to emit a helper call surrounded by memory barriers, so we need to spill any side
                    // effects.
                    impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("spilling side-effects"));
                }
#endif

                op3 = gtFoldExpr(impPopStack().val); // Size
                op2 = gtFoldExpr(impPopStack().val); // Value / Src addr
                op1 = impPopStack().val;             // Dst addr

                if (op3->IsCnsIntOrI())
                {
                    if (op3->IsIntegralConst(0))
                    {
                        if ((op1->gtFlags & GTF_SIDE_EFFECT) != 0)
                        {
                            impAppendTree(gtUnusedValNode(op1), CHECK_SPILL_ALL, impCurStmtDI);
                        }

                        if ((op2->gtFlags & GTF_SIDE_EFFECT) != 0)
                        {
                            impAppendTree(gtUnusedValNode(op2), CHECK_SPILL_ALL, impCurStmtDI);
                        }

                        break;
                    }

                    ClassLayout* layout = typGetBlkLayout(static_cast<unsigned>(op3->AsIntConCommon()->IconValue()));

                    if (opcode == CEE_INITBLK)
                    {
                        if (!op2->IsIntegralConst(0))
                        {
                            op2 = gtNewOperNode(GT_INIT_VAL, TYP_INT, op2);
                        }
                    }
                    else
                    {
                        op2 = gtNewLoadValueNode(layout, op2, indirFlags);
                    }

                    op1 = gtNewStoreValueNode(layout, op1, op2, indirFlags);
                }
                else
                {
                    if (TARGET_POINTER_SIZE == 8)
                    {
                        // Cast size to TYP_LONG on 64-bit targets
                        op3 = gtNewCastNode(TYP_LONG, op3, /* fromUnsigned */ true, TYP_LONG);
                    }

                    GenTreeCall* call;
                    if (opcode == CEE_INITBLK)
                    {
                        // value is zero -> memzero, otherwise -> memset
                        if (op2->IsIntegralConst(0))
                        {
                            call = gtNewHelperCallNode(CORINFO_HELP_MEMZERO, TYP_VOID, op1, op3);
                        }
                        else
                        {
                            call = gtNewHelperCallNode(CORINFO_HELP_MEMSET, TYP_VOID, op1, op2, op3);
                        }
                    }
                    else
                    {
                        call = gtNewHelperCallNode(CORINFO_HELP_MEMCPY, TYP_VOID, op1, op2, op3);
                    }

                    if (isVolatile)
                    {
                        // Wrap with memory barriers: store-barrier + call + load-barrier
                        impAppendTree(gtNewMemoryBarrier(BARRIER_STORE_ONLY), CHECK_SPILL_ALL, impCurStmtDI);
                        impAppendTree(call, CHECK_SPILL_ALL, impCurStmtDI);
                        op1 = gtNewMemoryBarrier(BARRIER_LOAD_ONLY);
                    }
                    else
                    {
                        op1 = call;
                    }
                }
                goto SPILL_APPEND;
            }

            case CEE_CPOBJ:
            {
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                ClassLayout* layout;
                lclTyp = TypeHandleToVarType(resolvedToken.hClass, &layout);

                if (lclTyp != TYP_STRUCT)
                {
                    op2 = impPopStack().val; // address to load from
                    op2 = gtNewIndir(lclTyp, op2);
                    goto STIND_VALUE;
                }

                op2 = impPopStack().val; // Src addr
                op1 = impPopStack().val; // Dest addr

                op2 = gtNewLoadValueNode(layout, op2);
                op1 = gtNewStoreValueNode(layout, op1, op2);
                goto SPILL_APPEND;
            }

            case CEE_STOBJ:
            {
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                ClassLayout* layout;
                lclTyp = TypeHandleToVarType(resolvedToken.hClass, &layout);

                if (!varTypeIsStruct(lclTyp))
                {
                    goto STIND;
                }

                op2 = impPopStack().val; // Value
                op1 = impPopStack().val; // Ptr
                assertImp(varTypeIsStruct(op2));

                GenTreeFlags indirFlags = impPrefixFlagsToIndirFlags(prefixFlags);
                if (eeIsByrefLike(resolvedToken.hClass))
                {
                    indirFlags |= GTF_IND_TGT_NOT_HEAP;
                }

                op1 = gtNewStoreValueNode(layout, op1, op2, indirFlags);

                op1 = impStoreStruct(op1, CHECK_SPILL_ALL);
                goto SPILL_APPEND;
            }

            case CEE_MKREFANY:
            {
                assert(!compIsForInlining());

                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

                op2 = impTokenToHandle(&resolvedToken, nullptr, true);
                if (op2 == nullptr)
                { // compDonotInline()
                    return;
                }

                accessAllowedResult =
                    info.compCompHnd->canAccessClass(&resolvedToken, info.compMethodHnd, &calloutHelper);
                impHandleAccessAllowed(accessAllowedResult, &calloutHelper);

                op1 = impPopStack().val;

                // @SPECVIOLATION: TYP_INT should not be allowed here by a strict reading of the spec.
                // But JIT32 allowed it, so we continue to allow it.
                assertImp(op1->TypeIs(TYP_BYREF, TYP_I_IMPL, TYP_INT));

                unsigned refAnyLcl = lvaGrabTemp(false DEBUGARG("mkrefany temp"));
                lvaSetStruct(refAnyLcl, impGetRefAnyClass(), false);

                GenTree* storeData =
                    gtNewStoreLclFldNode(refAnyLcl, op1->TypeGet(), OFFSETOF__CORINFO_TypedReference__dataPtr, op1);
                GenTree* storeType =
                    gtNewStoreLclFldNode(refAnyLcl, op2->TypeGet(), OFFSETOF__CORINFO_TypedReference__type, op2);
                impAppendTree(storeData, CHECK_SPILL_ALL, impCurStmtDI);
                impAppendTree(storeType, CHECK_SPILL_ALL, impCurStmtDI);

                impPushOnStack(gtNewLclVarNode(refAnyLcl, TYP_STRUCT), makeTypeInfo(impGetRefAnyClass()));
                break;
            }

            case CEE_LDOBJ:
            {
                assertImp(sz == sizeof(unsigned));

                _impResolveToken(CORINFO_TOKENKIND_Class);

                JITDUMP(" %08X", resolvedToken.token);

            OBJ:
                ClassLayout* layout;
                lclTyp   = TypeHandleToVarType(resolvedToken.hClass, &layout);
                tiRetVal = makeTypeInfo(resolvedToken.hClass);

                op1 = impPopStack().val;
                assertImp((genActualType(op1) == TYP_I_IMPL) || op1->TypeIs(TYP_BYREF));

                op1 = gtNewLoadValueNode(lclTyp, layout, op1, impPrefixFlagsToIndirFlags(prefixFlags));
                impPushOnStack(op1, tiRetVal);
                break;
            }

            case CEE_LDLEN:
                op1 = impPopStack().val;
                if (opts.OptimizationEnabled())
                {
                    /* Use GT_ARR_LENGTH operator so rng check opts see this */
                    GenTreeArrLen* arrLen = gtNewArrLen(TYP_INT, op1, OFFSETOF__CORINFO_Array__length);

                    op1 = arrLen;
                }
                else
                {
                    /* Create the expression "*(array_addr + ArrLenOffs)" */
                    op1 = gtNewOperNode(GT_ADD, TYP_BYREF, op1,
                                        gtNewIconNode(OFFSETOF__CORINFO_Array__length, TYP_I_IMPL));
                    op1 = gtNewIndir(TYP_INT, op1);
                }

                /* Push the result back on the stack */
                impPushOnStack(op1, tiRetVal);
                break;

            case CEE_BREAK:
                op1 = gtNewHelperCallNode(CORINFO_HELP_USER_BREAKPOINT, TYP_VOID);
                goto SPILL_APPEND;

            case CEE_NOP:
                if (opts.compDbgCode)
                {
                    op1 = new (this, GT_NO_OP) GenTree(GT_NO_OP, TYP_VOID);
                    goto SPILL_APPEND;
                }
                break;

                /******************************** NYI *******************************/

            case 0xCC:
                OutputDebugStringA("CLR: Invalid x86 breakpoint in IL stream\n");
                FALLTHROUGH;

            case CEE_ILLEGAL:
            case CEE_MACRO_END:

            default:
                if (compIsForInlining())
                {
                    compInlineResult->NoteFatal(InlineObservation::CALLEE_COMPILATION_ERROR);
                    return;
                }

                BADCODE3("unknown opcode", ": %02X", (int)opcode);
        }

        codeAddr += sz;
        prevOpcode = opcode;

        prefixFlags = 0;
    }

    return;
#undef _impResolveToken
}

//------------------------------------------------------------------------
// impCreateLocal: create a GT_LCL_VAR node to access a local that might need to be normalized on load
//
// Arguments:
//     lclNum -- The index into lvaTable
//     offset -- The offset to associate with the node
//
// Returns:
//     The node
//
GenTreeLclVar* Compiler::impCreateLocalNode(unsigned lclNum DEBUGARG(IL_OFFSET offset))
{
    var_types lclTyp;

    if (lvaTable[lclNum].lvNormalizeOnLoad())
    {
        lclTyp = lvaGetRealType(lclNum);
    }
    else
    {
        lclTyp = lvaGetActualType(lclNum);
    }

    return gtNewLclvNode(lclNum, lclTyp DEBUGARG(offset));
}

// Load a local/argument on the operand stack
// lclNum is an index into lvaTable *NOT* the arg/lcl index in the IL
void Compiler::impLoadVar(unsigned lclNum, IL_OFFSET offset)
{
    impPushOnStack(impCreateLocalNode(lclNum DEBUGARG(offset)), makeTypeInfoForLocal(lclNum));
}

// Load an argument on the operand stack
// Shared by the various CEE_LDARG opcodes
// ilArgNum is the argument index as specified in IL.
// It will be mapped to the correct lvaTable index
void Compiler::impLoadArg(unsigned ilArgNum, IL_OFFSET offset)
{
    if (compIsForInlining())
    {
        if (ilArgNum >= info.compArgsCount)
        {
            compInlineResult->NoteFatal(InlineObservation::CALLEE_BAD_ARGUMENT_NUMBER);
            return;
        }

        var_types type = impInlineInfo->lclVarInfo[ilArgNum].lclTypeInfo;
        typeInfo  tiRetVal;
        if (type == TYP_REF)
        {
            tiRetVal = typeInfo(impInlineInfo->lclVarInfo[ilArgNum].lclTypeHandle);
        }
        else
        {
            tiRetVal = typeInfo(type);
        }

        impPushOnStack(impInlineFetchArg(impInlineInfo->inlArgInfo[ilArgNum], impInlineInfo->lclVarInfo[ilArgNum]),
                       tiRetVal);
    }
    else
    {
        if (ilArgNum >= info.compArgsCount)
        {
            BADCODE("Bad IL");
        }

        unsigned lclNum = compMapILargNum(ilArgNum); // account for possible hidden param

        if (lclNum == info.compThisArg)
        {
            lclNum = lvaArg0Var;
        }

        impLoadVar(lclNum, offset);
    }
}

// Load a local on the operand stack
// Shared by the various CEE_LDLOC opcodes
// ilLclNum is the local index as specified in IL.
// It will be mapped to the correct lvaTable index
void Compiler::impLoadLoc(unsigned ilLclNum, IL_OFFSET offset)
{
    unsigned lclNum;
    if (compIsForInlining())
    {
        if (ilLclNum >= info.compMethodInfo->locals.numArgs)
        {
            compInlineResult->NoteFatal(InlineObservation::CALLEE_BAD_LOCAL_NUMBER);
            return;
        }

        // Have we allocated a temp for this local?
        lclNum = impInlineFetchLocal(ilLclNum DEBUGARG("Inline ldloc first use temp"));
    }
    else
    {
        if (ilLclNum >= info.compMethodInfo->locals.numArgs)
        {
            BADCODE("Bad IL");
        }

        lclNum = info.compArgsCount + ilLclNum;
    }

    impLoadVar(lclNum, offset);
}

//------------------------------------------------------------------------
// impStoreMultiRegValueToVar: ensure calls that return structs in multiple
//    registers return values to suitable temps.
//
// Arguments:
//     op -- call returning a struct in registers
//     hClass -- class handle for struct
//
// Returns:
//     Tree with reference to struct local to use as call return value.

GenTree* Compiler::impStoreMultiRegValueToVar(GenTree*                    op,
                                              CORINFO_CLASS_HANDLE hClass DEBUGARG(CorInfoCallConvExtension callConv))
{
    unsigned tmpNum = lvaGrabTemp(true DEBUGARG("Return value temp for multireg return"));
    lvaSetStruct(tmpNum, hClass, false);

    impStoreToTemp(tmpNum, op, CHECK_SPILL_ALL);

    LclVarDsc* varDsc = lvaGetDesc(tmpNum);

    varDsc->SetIsMultiRegDest();

    GenTreeLclVar* ret = gtNewLclvNode(tmpNum, varDsc->lvType);

    // TODO-1stClassStructs: Handle constant propagation and CSE-ing of multireg returns.
    ret->SetDoNotCSE();

    assert(IsMultiRegReturnedType(hClass, callConv) || op->IsMultiRegNode());

    return ret;
}

//------------------------------------------------------------------------
// impReturnInstruction: import a return or an explicit tail call
//
// Arguments:
//     prefixFlags -- active IL prefixes
//     opcode -- [in, out] IL opcode
//
// Returns:
//     True if import was successful (may fail for some inlinees)
//
bool Compiler::impReturnInstruction(int prefixFlags, OPCODE& opcode)
{
    const bool isTailCall = (prefixFlags & PREFIX_TAILCALL) != 0;

#ifdef DEBUG
    // If we are importing an inlinee and have GC ref locals we always
    // need to have a spill temp for the return value.  This temp
    // should have been set up in advance, over in fgFindBasicBlocks.
    if (compIsForInlining() && impInlineInfo->HasGcRefLocals() && (info.compRetType != TYP_VOID))
    {
        assert(lvaInlineeReturnSpillTemp != BAD_VAR_NUM);
    }

    if (!compIsForInlining() && ((prefixFlags & (PREFIX_TAILCALL_EXPLICIT | PREFIX_TAILCALL_STRESS)) == 0) &&
        compStressCompile(STRESS_POISON_IMPLICIT_BYREFS, 25))
    {
        impPoisonImplicitByrefsBeforeReturn();
    }
#endif // DEBUG

    GenTree* op2 = nullptr;
    GenTree* op1 = nullptr;

    if (info.compRetType != TYP_VOID)
    {
        op2 = impPopStack().val;

        if (!compIsForInlining())
        {
            impBashVarAddrsToI(op2);
            op2 = impImplicitIorI4Cast(op2, info.compRetType);
            op2 = impImplicitR4orR8Cast(op2, info.compRetType);

            assertImp((genActualType(op2->TypeGet()) == genActualType(info.compRetType)) ||
                      (op2->TypeIs(TYP_I_IMPL) && (info.compRetType == TYP_BYREF)) ||
                      (op2->TypeIs(TYP_BYREF) && (info.compRetType == TYP_I_IMPL)) ||
                      (varTypeIsFloating(op2->gtType) && varTypeIsFloating(info.compRetType)) ||
                      (varTypeIsStruct(op2) && varTypeIsStruct(info.compRetType)));

#ifdef DEBUG
            if (!isTailCall && opts.compGcChecks && (info.compRetType == TYP_REF))
            {
                // DDB 3483  : JIT Stress: early termination of GC ref's life time in exception code path
                // VSW 440513: Incorrect gcinfo on the return value under DOTNET_JitGCChecks=1 for methods with
                // one-return BB.

                assert(op2->TypeIs(TYP_REF));

                // confirm that the argument is a GC pointer (for debugging (GC stress))
                op2 = gtNewHelperCallNode(CORINFO_HELP_CHECK_OBJ, TYP_REF, op2);

                if (verbose)
                {
                    printf("\ncompGcChecks tree:\n");
                    gtDispTree(op2);
                }
            }
#endif
        }
        else
        {
            if (stackState.esStackDepth != 0)
            {
                assert(compIsForInlining());
                JITDUMP("CALLSITE_COMPILATION_ERROR: inlinee's stack is not empty.");
                compInlineResult->NoteFatal(InlineObservation::CALLSITE_COMPILATION_ERROR);
                return false;
            }

#ifdef DEBUG
            if (verbose)
            {
                printf("\n\n    Inlinee Return expression (before normalization)  =>\n");
                gtDispTree(op2);
            }
#endif

            InlineCandidateInfo* inlCandInfo = impInlineInfo->inlineCandidateInfo;
            GenTreeRetExpr*      inlRetExpr  = inlCandInfo->retExpr;
            // Make sure the type matches the original call.

            var_types returnType       = genActualType(op2->gtType);
            var_types originalCallType = genActualType(JITtype2varType(inlCandInfo->methInfo.args.retType));
            if ((returnType != originalCallType) && (originalCallType == TYP_STRUCT))
            {
                originalCallType = impNormStructType(inlCandInfo->methInfo.args.retTypeClass);
            }

            if (returnType != originalCallType)
            {
                // Allow TYP_BYREF to be returned as TYP_I_IMPL and vice versa.
                if (((returnType == TYP_BYREF) && (originalCallType == TYP_I_IMPL)) ||
                    ((returnType == TYP_I_IMPL) && (originalCallType == TYP_BYREF)))
                {
                    JITDUMP("Allowing return type mismatch: have %s, needed %s\n", varTypeName(returnType),
                            varTypeName(originalCallType));
                }
                else
                {
                    JITDUMP("Return type mismatch: have %s, needed %s\n", varTypeName(returnType),
                            varTypeName(originalCallType));
                    compInlineResult->NoteFatal(InlineObservation::CALLSITE_RETURN_TYPE_MISMATCH);
                    return false;
                }
            }

            // Below, we are going to set impInlineInfo->retExpr to the tree with the return
            // expression. At this point, retExpr could already be set if there are multiple
            // return blocks (meaning fgNeedReturnSpillTemp() == true) and one of
            // the other blocks already set it. If there is only a single return block,
            // retExpr shouldn't be set. However, this is not true if we reimport a block
            // with a return. In that case, retExpr will be set, then the block will be
            // reimported, but retExpr won't get cleared as part of setting the block to
            // be reimported. The reimported retExpr value should be the same, so even if
            // we don't unconditionally overwrite it, it shouldn't matter.
            if (info.compRetNativeType != TYP_STRUCT)
            {
                // compRetNativeType is not TYP_STRUCT.
                // This implies it could be either a scalar type or SIMD vector type or
                // a struct type that can be normalized to a scalar type.

                if (varTypeIsStruct(info.compRetType))
                {
                    noway_assert(info.compRetBuffArg == BAD_VAR_NUM);
                    // Handle calls with "fake" return buffers.
                    op2 = impFixupStructReturnType(op2);
                }
                else
                {
                    // Do we have to normalize?
                    var_types fncRealRetType = JITtype2varType(info.compMethodInfo->args.retType);
                    // For RET_EXPR get the type info from the call. Regardless
                    // of whether it ends up inlined or not normalization will
                    // happen as part of that function's codegen.
                    GenTree* returnedTree = op2->OperIs(GT_RET_EXPR) ? op2->AsRetExpr()->gtInlineCandidate : op2;
                    if ((varTypeIsSmall(returnedTree->TypeGet()) || varTypeIsSmall(fncRealRetType)) &&
                        fgCastNeeded(returnedTree, fncRealRetType))
                    {
                        // Small-typed return values are normalized by the callee
                        op2 = gtNewCastNode(TYP_INT, op2, false, fncRealRetType);
                    }
                }

                if (fgNeedReturnSpillTemp())
                {
                    assert(info.compRetNativeType != TYP_VOID &&
                           (fgMoreThanOneReturnBlock() || impInlineInfo->HasGcRefLocals()));

                    // If this method returns a ref type, track the actual types seen in the returns.
                    if (info.compRetType == TYP_REF)
                    {
                        bool                 isExact      = false;
                        bool                 isNonNull    = false;
                        CORINFO_CLASS_HANDLE returnClsHnd = gtGetClassHandle(op2, &isExact, &isNonNull);

                        if (inlRetExpr->gtSubstExpr == nullptr)
                        {
                            // This is the first return, so best known type is the type
                            // of this return value.
                            impInlineInfo->retExprClassHnd        = returnClsHnd;
                            impInlineInfo->retExprClassHndIsExact = isExact;
                        }
                        else
                        {
                            if (impInlineInfo->retExprClassHnd != returnClsHnd)
                            {
                                // This return site type differs from earlier seen sites,
                                // so reset the info and we'll fall back to using the method's
                                // declared return type for the return spill temp.
                                impInlineInfo->retExprClassHnd        = nullptr;
                                impInlineInfo->retExprClassHndIsExact = false;
                            }
                            else
                            {
                                // Same return type, but we may need to update exactness.
                                impInlineInfo->retExprClassHndIsExact &= isExact;
                            }
                        }
                    }

                    impStoreToTemp(lvaInlineeReturnSpillTemp, op2, CHECK_SPILL_ALL);

                    var_types lclRetType = lvaGetDesc(lvaInlineeReturnSpillTemp)->lvType;
                    GenTree*  tmpOp2     = gtNewLclvNode(lvaInlineeReturnSpillTemp, lclRetType);

                    op2 = tmpOp2;
#ifdef DEBUG
                    if (inlRetExpr->gtSubstExpr != nullptr)
                    {
                        // Some other block(s) have seen the CEE_RET first.
                        // Better they spilled to the same temp.
                        assert(inlRetExpr->gtSubstExpr->OperIs(GT_LCL_VAR));
                        assert(inlRetExpr->gtSubstExpr->AsLclVarCommon()->GetLclNum() ==
                               op2->AsLclVarCommon()->GetLclNum());
                    }
#endif
                }

#ifdef DEBUG
                if (verbose)
                {
                    printf("\n\n    Inlinee Return expression (after normalization) =>\n");
                    gtDispTree(op2);
                }
#endif

                // Report the return expression
                inlRetExpr->gtSubstExpr = op2;
            }
            else
            {
                // compRetNativeType is TYP_STRUCT.
                // This implies that struct return via RetBuf arg or multi-reg struct return.

                GenTreeCall* iciCall = impInlineInfo->iciCall->AsCall();

                // Assign the inlinee return into a spill temp.
                if (fgNeedReturnSpillTemp())
                {
                    // in this case we have to insert multiple struct copies to the temp
                    // and the retexpr is just the temp.
                    assert(info.compRetNativeType != TYP_VOID);
                    assert(fgMoreThanOneReturnBlock() || impInlineInfo->HasGcRefLocals());

                    impStoreToTemp(lvaInlineeReturnSpillTemp, op2, CHECK_SPILL_ALL);
                }

                if (compMethodReturnsMultiRegRetType())
                {
                    assert(!iciCall->ShouldHaveRetBufArg());

                    if (fgNeedReturnSpillTemp())
                    {
                        if (inlRetExpr->gtSubstExpr == nullptr)
                        {
                            // The inlinee compiler has figured out the type of the temp already. Use it here.
                            inlRetExpr->gtSubstExpr =
                                gtNewLclvNode(lvaInlineeReturnSpillTemp, lvaTable[lvaInlineeReturnSpillTemp].lvType);
                        }
                    }
                    else
                    {
                        inlRetExpr->gtSubstExpr = op2;
                    }
                }
                else // The struct was to be returned via a return buffer.
                {
                    assert(iciCall->gtArgs.HasRetBuffer());
                    GenTree* dest = gtCloneExpr(iciCall->gtArgs.GetRetBufferArg()->GetEarlyNode());

                    if (fgNeedReturnSpillTemp())
                    {
                        // If this is the first return we have seen set the retExpr.
                        if (inlRetExpr->gtSubstExpr == nullptr)
                        {
                            inlRetExpr->gtSubstExpr =
                                impStoreStructPtr(dest, gtNewLclvNode(lvaInlineeReturnSpillTemp, info.compRetType),
                                                  CHECK_SPILL_ALL);
                        }
                    }
                    else
                    {
                        inlRetExpr->gtSubstExpr = impStoreStructPtr(dest, op2, CHECK_SPILL_ALL);
                    }
                }
            }

            // If gtSubstExpr is an arbitrary tree then we may need to
            // propagate mandatory "IR presence" flags to the BB it ends up in.
            inlRetExpr->gtSubstBB = fgNeedReturnSpillTemp() ? nullptr : compCurBB;
        }
    }

    if (compIsForInlining())
    {
        return true;
    }

    if (info.compRetBuffArg != BAD_VAR_NUM)
    {
        var_types retBuffType = lvaGetDesc(info.compRetBuffArg)->TypeGet();
        // Assign value to return buff (first param)
        GenTree* retBuffAddr =
            gtNewLclvNode(info.compRetBuffArg, retBuffType DEBUGARG(impCurStmtDI.GetLocation().GetOffset()));

        op2 = impStoreStructPtr(retBuffAddr, op2, CHECK_SPILL_ALL, GTF_IND_TGT_NOT_HEAP);
        impAppendTree(op2, CHECK_SPILL_NONE, impCurStmtDI);

        // There are cases where the address of the implicit RetBuf should be returned explicitly.
        //
        if (compMethodReturnsRetBufAddr())
        {
            op1 = gtNewOperNode(GT_RETURN, retBuffType, gtNewLclvNode(info.compRetBuffArg, retBuffType));
        }
        else
        {
            op1 = new (this, GT_RETURN) GenTreeOp(GT_RETURN, TYP_VOID);
        }
    }
    else if (varTypeIsStruct(info.compRetType))
    {
#if !FEATURE_MULTIREG_RET
        // For both ARM architectures the HFA native types are maintained as structs.
        // Also on System V AMD64 the multireg structs returns are also left as structs.
        noway_assert(info.compRetNativeType != TYP_STRUCT);
#endif
        op2 = impFixupStructReturnType(op2);
        op1 = gtNewOperNode(GT_RETURN, genActualType(info.compRetType), op2);
    }
    else if (info.compRetType != TYP_VOID)
    {
        op1 = gtNewOperNode(GT_RETURN, genActualType(info.compRetType), op2);
    }
    else
    {
        op1 = new (this, GT_RETURN) GenTreeOp(GT_RETURN, TYP_VOID);
    }

    // We must have imported a tailcall and jumped to RET
    if (isTailCall)
    {
        assert(stackState.esStackDepth == 0 && impOpcodeIsCallOpcode(opcode));

        opcode = CEE_RET; // To prevent trying to spill if CALL_SITE_BOUNDARIES

        // impImportCall() would have already appended TYP_VOID calls
        if (info.compRetType == TYP_VOID)
        {
            return true;
        }
    }

    impAppendTree(op1, CHECK_SPILL_NONE, impCurStmtDI);
#ifdef DEBUG
    // Remember at which BC offset the tree was finished
    impNoteLastILoffs();
#endif
    return true;
}

#ifdef DEBUG
//------------------------------------------------------------------------
// impPoisonImplicitByrefsBeforeReturn:
//   Spill the stack and insert IR that poisons all implicit byrefs.
//
// Remarks:
//   The memory pointed to by implicit byrefs is owned by the callee but
//   usually exists on the caller's frame (or on the heap for some reflection
//   invoke scenarios). This function helps catch situations where the caller
//   reads from the memory after the invocation, for example due to a bug in
//   the JIT's own last-use copy elision for implicit byrefs.
//
void Compiler::impPoisonImplicitByrefsBeforeReturn()
{
    bool spilled = false;
    for (unsigned lclNum = 0; lclNum < info.compArgsCount; lclNum++)
    {
        if (!lvaIsImplicitByRefLocal(lclNum))
        {
            continue;
        }

        compPoisoningAnyImplicitByrefs = true;

        if (!spilled)
        {
            for (unsigned level = 0; level < stackState.esStackDepth; level++)
            {
                impSpillStackEntry(level, BAD_VAR_NUM DEBUGARG(true) DEBUGARG("Stress poisoning byrefs before return"));
            }

            spilled = true;
        }

        LclVarDsc* dsc = lvaGetDesc(lclNum);
        // Be conservative about this local to ensure we do not eliminate the poisoning.
        lvaSetVarAddrExposed(lclNum, AddressExposedReason::STRESS_POISON_IMPLICIT_BYREFS);

        assert(varTypeIsStruct(dsc));
        ClassLayout* layout = dsc->GetLayout();
        assert(layout != nullptr);

        auto poisonBlock = [this, lclNum](unsigned start, unsigned count) {
            if (count <= 0)
            {
                return;
            }

            GenTree* initValue = gtNewOperNode(GT_INIT_VAL, TYP_INT, gtNewIconNode(0xcd));
            GenTree* store     = gtNewStoreLclFldNode(lclNum, TYP_STRUCT, typGetBlkLayout(count), start, initValue);
            impAppendTree(store, CHECK_SPILL_NONE, DebugInfo());
        };

        unsigned startOffs = 0;
        unsigned numSlots  = layout->GetSlotCount();
        for (unsigned curSlot = 0; curSlot < numSlots; curSlot++)
        {
            unsigned  offs  = curSlot * TARGET_POINTER_SIZE;
            var_types gcPtr = layout->GetGCPtrType(curSlot);
            if (!varTypeIsGC(gcPtr))
            {
                continue;
            }

            poisonBlock(startOffs, offs - startOffs);

            GenTree* zeroField = gtNewStoreLclFldNode(lclNum, gcPtr, offs, gtNewZeroConNode(gcPtr));
            impAppendTree(zeroField, CHECK_SPILL_NONE, DebugInfo());

            startOffs = offs + TARGET_POINTER_SIZE;
        }

        assert(startOffs <= lvaLclExactSize(lclNum));
        poisonBlock(startOffs, lvaLclExactSize(lclNum) - startOffs);
    }
}
#endif

/*****************************************************************************
 *  Mark the block as unimported.
 *  Note that the caller is responsible for calling impImportBlockPending(),
 *  with the appropriate stack-state
 */

inline void Compiler::impReimportMarkBlock(BasicBlock* block)
{
#ifdef DEBUG
    if (verbose && block->HasFlag(BBF_IMPORTED))
    {
        printf("\n" FMT_BB " will be reimported\n", block->bbNum);
    }
#endif

    // We shouldn't be re-importing one of these special blocks.
    assert(!block->KindIs(BBJ_CALLFINALLYRET));

    if (block->isBBCallFinallyPair())
    {
        // If we're going to re-import a BBJ_CALLFINALLY that has a paired BBJ_CALLFINALLYRET,
        // remove the BBJ_CALLFINALLYRET.
        BasicBlock* const leaveBlock = block->Next();
        fgPrepareCallFinallyRetForRemoval(leaveBlock);
        fgRemoveBlock(leaveBlock, /* unreachable */ true);

        // The above code marked the BBJ_CALLFINALLY as retless. Remove that.
        block->RemoveFlags(BBF_RETLESS_CALL);
    }

    block->RemoveFlags(BBF_IMPORTED);
}

void Compiler::impVerifyEHBlock(BasicBlock* block)
{
    assert(block->hasTryIndex());
    assert(!compIsForInlining() || opts.compInlineMethodsWithEH);

    unsigned  tryIndex = block->getTryIndex();
    EHblkDsc* HBtab    = ehGetDsc(tryIndex);

    if (bbIsTryBeg(block) && (block->bbStkDepth != 0))
    {
        BADCODE("Evaluation stack must be empty on entry into a try block");
    }

    // Save the stack contents, we'll need to restore it later
    //
    SavedStack blockState;
    impSaveStackState(&blockState, false);

    while (HBtab != nullptr)
    {
        // Recursively process the handler block, if we haven't already done so.
        BasicBlock* hndBegBB = HBtab->ebdHndBeg;

        if (!hndBegBB->HasFlag(BBF_IMPORTED) && (impGetPendingBlockMember(hndBegBB) == 0))
        {
            //  Construct the proper verification stack state
            //   either empty or one that contains just
            //   the Exception Object that we are dealing with
            //
            stackState.esStackDepth = 0;

            if (handlerGetsXcptnObj(hndBegBB->bbCatchTyp))
            {
                CORINFO_CLASS_HANDLE clsHnd;

                if (HBtab->HasFilter())
                {
                    clsHnd = impGetObjectClass();
                }
                else
                {
                    CORINFO_RESOLVED_TOKEN resolvedToken;

                    resolvedToken.tokenContext = impTokenLookupContextHandle;
                    resolvedToken.tokenScope   = info.compScopeHnd;
                    resolvedToken.token        = HBtab->ebdTyp;
                    resolvedToken.tokenType    = CORINFO_TOKENKIND_Class;
                    info.compCompHnd->resolveToken(&resolvedToken);

                    clsHnd = resolvedToken.hClass;
                }

                // push catch arg the stack, spill to a temp if necessary
                // Note: can update HBtab->ebdHndBeg!
                hndBegBB = impPushCatchArgOnStack(hndBegBB, clsHnd, false);
            }

            // Queue up the handler for importing
            //
            impImportBlockPending(hndBegBB);
        }

        // Process the filter block, if we haven't already done so.
        if (HBtab->HasFilter())
        {
            BasicBlock* filterBB = HBtab->ebdFilter;

            if (!filterBB->HasFlag(BBF_IMPORTED) && (impGetPendingBlockMember(filterBB) == 0))
            {
                stackState.esStackDepth = 0;

                // push catch arg the stack, spill to a temp if necessary
                // Note: can update HBtab->ebdFilter!
                const bool isSingleBlockFilter = (filterBB->NextIs(hndBegBB));
                filterBB = impPushCatchArgOnStack(filterBB, impGetObjectClass(), isSingleBlockFilter);

                impImportBlockPending(filterBB);
            }
        }

        // Now process our enclosing try index (if any)
        //
        tryIndex = HBtab->ebdEnclosingTryIndex;
        if (tryIndex == EHblkDsc::NO_ENCLOSING_INDEX)
        {
            HBtab = nullptr;
        }
        else
        {
            HBtab = ehGetDsc(tryIndex);
        }
    }

    // Restore the stack contents
    impRestoreStackState(&blockState);
}

//***************************************************************
// Import the instructions for the given basic block.  Perform
// verification, throwing an exception on failure.  Push any successor blocks that are enabled for the first
// time, or whose verification pre-state is changed.
void Compiler::impImportBlock(BasicBlock* block)
{
    // BBF_INTERNAL blocks only exist during importation due to EH canonicalization. We need to
    // handle them specially. In particular, there is no IL to import for them, but we do need
    // to mark them as imported and put their successors on the pending import list.
    if (block->HasFlag(BBF_INTERNAL))
    {
        JITDUMP("Marking BBF_INTERNAL block " FMT_BB " as BBF_IMPORTED\n", block->bbNum);
        block->SetFlags(BBF_IMPORTED);

        for (BasicBlock* const succBlock : block->Succs())
        {
            impImportBlockPending(succBlock);
        }

        return;
    }

    bool markImport;

    assert(block);

    /* Make the block globally available */

    compCurBB = block;

#ifdef DEBUG
    /* Initialize the debug variables */
    impCurOpcName = "unknown";
    impCurOpcOffs = block->bbCodeOffs;
#endif

    /* Set the current stack state to the merged result */
    resetCurrentState(block, &stackState);

    if (block->hasTryIndex())
    {
        impVerifyEHBlock(block);
    }

    // Now walk the code and import the IL into GenTrees.
    impImportBlockCode(block);

    if (compDonotInline())
    {
        return;
    }

    assert(!compDonotInline());

    markImport = false;

SPILLSTACK:

    unsigned    baseTmp             = NO_BASE_TMP; // input temps assigned to successor blocks
    bool        reimportSpillClique = false;
    BasicBlock* tgtBlock            = nullptr;

    /* If the stack is non-empty, we might have to spill its contents */

    if (stackState.esStackDepth != 0)
    {
        impBoxTemp = BAD_VAR_NUM; // if a box temp is used in a block that leaves something
                                  // on the stack, its lifetime is hard to determine, simply
                                  // don't reuse such temps.

        Statement* addStmt = nullptr;

        /* Do the successors of 'block' have any other predecessors ?
           We do not want to do some of the optimizations related to multiRef
           if we can reimport blocks */

        unsigned multRef = impCanReimport ? unsigned(~0) : 0;

        switch (block->GetKind())
        {
            case BBJ_COND:

                addStmt = impExtractLastStmt();

                assert(addStmt->GetRootNode()->OperIs(GT_JTRUE));

                /* Note if the next block has more than one ancestor */

                multRef |= block->GetFalseTarget()->bbRefs;

                /* Does the next block have temps assigned? */

                baseTmp  = block->GetFalseTarget()->bbStkTempsIn;
                tgtBlock = block->GetFalseTarget();

                if (baseTmp != NO_BASE_TMP)
                {
                    break;
                }

                /* Try the target of the jump then */

                multRef |= block->GetTrueTarget()->bbRefs;
                baseTmp  = block->GetTrueTarget()->bbStkTempsIn;
                tgtBlock = block->GetTrueTarget();
                break;

            case BBJ_ALWAYS:
                multRef |= block->GetTarget()->bbRefs;
                baseTmp  = block->GetTarget()->bbStkTempsIn;
                tgtBlock = block->GetTarget();
                break;

            case BBJ_SWITCH:
                addStmt = impExtractLastStmt();
                assert(addStmt->GetRootNode()->OperIs(GT_SWITCH));

                for (BasicBlock* const tgtBlock : block->SwitchSuccs())
                {
                    multRef |= tgtBlock->bbRefs;

                    // Thanks to spill cliques, we should have assigned all or none
                    assert((baseTmp == NO_BASE_TMP) || (baseTmp == tgtBlock->bbStkTempsIn));
                    baseTmp = tgtBlock->bbStkTempsIn;
                    if (multRef > 1)
                    {
                        break;
                    }
                }
                break;

            case BBJ_CALLFINALLY:
            case BBJ_EHCATCHRET:
            case BBJ_RETURN:
            case BBJ_EHFINALLYRET:
            case BBJ_EHFAULTRET:
            case BBJ_EHFILTERRET:
            case BBJ_THROW:
                BADCODE("can't have 'unreached' end of BB with non-empty stack");
                break;

            default:
                noway_assert(!"Unexpected bbKind");
                break;
        }

        assert(multRef >= 1);

        /* Do we have a base temp number? */

        bool newTemps = (baseTmp == NO_BASE_TMP);

        if (newTemps)
        {
            /* Grab enough temps for the whole stack */
            baseTmp = impGetSpillTmpBase(block);
        }

        // Spill all stack entries into temps

        JITDUMP("\nSpilling stack entries into temps\n");
        for (unsigned level = 0, tempNum = baseTmp; level < stackState.esStackDepth; level++, tempNum++)
        {
            GenTree* tree = stackState.esStack[level].val;

            // VC generates code where it pushes a byref from one branch, and an int (ldc.i4 0) from
            // the other. This should merge to a byref in unverifiable code.
            // However, if the branch which leaves the TYP_I_IMPL on the stack is imported first, the
            // successor would be imported assuming there was a TYP_I_IMPL on
            // the stack. Thus the value would not get GC-tracked. Hence,
            // change the temp to TYP_BYREF and reimport the clique.
            LclVarDsc* tempDsc = lvaGetDesc(tempNum);
            if (tree->TypeIs(TYP_BYREF) && tempDsc->TypeIs(TYP_I_IMPL))
            {
                tempDsc->lvType     = TYP_BYREF;
                reimportSpillClique = true;
            }

#ifdef TARGET_64BIT
            if ((genActualType(tree) == TYP_I_IMPL) && tempDsc->TypeIs(TYP_INT))
            {
                // Some other block in the spill clique set this to "int", but now we have "native int".
                // Change the type and go back to re-import any blocks that used the wrong type.
                tempDsc->lvType     = TYP_I_IMPL;
                reimportSpillClique = true;
            }
            else if ((genActualType(tree) == TYP_INT) && tempDsc->TypeIs(TYP_I_IMPL))
            {
                // Spill clique has decided this should be "native int", but this block only pushes an "int".
                // Insert a sign-extension to "native int" so we match the clique.
                stackState.esStack[level].val = gtNewCastNode(TYP_I_IMPL, tree, false, TYP_I_IMPL);
            }

            // Consider the case where one branch left a 'byref' on the stack and the other leaves
            // an 'int'. On 32-bit, this is allowed (in non-verifiable code) since they are the same
            // size. JIT64 managed to make this work on 64-bit. For compatibility, we support JIT64
            // behavior instead of asserting and then generating bad code (where we save/restore the
            // low 32 bits of a byref pointer to an 'int' sized local). If the 'int' side has been
            // imported already, we need to change the type of the local and reimport the spill clique.
            // If the 'byref' side has imported, we insert a cast from int to 'native int' to match
            // the 'byref' size.
            if ((genActualType(tree) == TYP_BYREF) && tempDsc->TypeIs(TYP_INT))
            {
                // Some other block in the spill clique set this to "int", but now we have "byref".
                // Change the type and go back to re-import any blocks that used the wrong type.
                tempDsc->lvType     = TYP_BYREF;
                reimportSpillClique = true;
            }
            else if ((genActualType(tree) == TYP_INT) && tempDsc->TypeIs(TYP_BYREF))
            {
                // Spill clique has decided this should be "byref", but this block only pushes an "int".
                // Insert a sign-extension to "native int" so we match the clique size.
                stackState.esStack[level].val = gtNewCastNode(TYP_I_IMPL, tree, false, TYP_I_IMPL);
            }

#endif // TARGET_64BIT

            if (tree->TypeIs(TYP_DOUBLE) && (tempDsc->lvType == TYP_FLOAT))
            {
                // Some other block in the spill clique set this to "float", but now we have "double".
                // Change the type and go back to re-import any blocks that used the wrong type.
                tempDsc->lvType     = TYP_DOUBLE;
                reimportSpillClique = true;
            }
            else if (tree->TypeIs(TYP_FLOAT) && tempDsc->TypeIs(TYP_DOUBLE))
            {
                // Spill clique has decided this should be "double", but this block only pushes a "float".
                // Insert a cast to "double" so we match the clique.
                stackState.esStack[level].val = gtNewCastNode(TYP_DOUBLE, tree, false, TYP_DOUBLE);
            }

            /* If addStmt has a reference to tempNum (can only happen if we
               are spilling to the temps already used by a previous block),
               we need to spill addStmt */

            if ((addStmt != nullptr) && !newTemps && gtHasRef(addStmt->GetRootNode(), tempNum))
            {
                GenTree* addTree = addStmt->GetRootNode();

                if (addTree->OperIs(GT_JTRUE))
                {
                    GenTree* relOp = addTree->AsOp()->gtOp1;
                    assert(relOp->OperIsCompare());

                    var_types type = genActualType(relOp->AsOp()->gtOp1->TypeGet());

                    if (gtHasRef(relOp->AsOp()->gtOp1, tempNum))
                    {
                        unsigned temp = lvaGrabTemp(true DEBUGARG("spill addStmt JTRUE ref Op1"));
                        impStoreToTemp(temp, relOp->AsOp()->gtOp1, level);
                        type                 = genActualType(lvaTable[temp].TypeGet());
                        relOp->AsOp()->gtOp1 = gtNewLclvNode(temp, type);
                    }

                    if (gtHasRef(relOp->AsOp()->gtOp2, tempNum))
                    {
                        unsigned temp = lvaGrabTemp(true DEBUGARG("spill addStmt JTRUE ref Op2"));
                        impStoreToTemp(temp, relOp->AsOp()->gtOp2, level);
                        type                 = genActualType(lvaTable[temp].TypeGet());
                        relOp->AsOp()->gtOp2 = gtNewLclvNode(temp, type);
                    }
                }
                else
                {
                    assert(addTree->OperIs(GT_SWITCH) && genActualTypeIsIntOrI(addTree->AsOp()->gtOp1->TypeGet()));

                    unsigned temp = lvaGrabTemp(true DEBUGARG("spill addStmt SWITCH"));
                    impStoreToTemp(temp, addTree->AsOp()->gtOp1, level);
                    addTree->AsOp()->gtOp1 = gtNewLclvNode(temp, genActualType(addTree->AsOp()->gtOp1->TypeGet()));
                }
            }

            /* Spill the stack entry, and replace with the temp */

            if (!impSpillStackEntry(level, tempNum
#ifdef DEBUG
                                    ,
                                    true, "Spill Stack Entry"
#endif
                                    ))
            {
                if (markImport)
                {
                    BADCODE("bad stack state");
                }

                goto SPILLSTACK;
            }
        }

        /* Put back the 'jtrue'/'switch' if we removed it earlier */

        if (addStmt != nullptr)
        {
            impAppendStmt(addStmt, CHECK_SPILL_NONE);
        }
    }

    // Some of the append/spill logic works on compCurBB

    assert(compCurBB == block);

    /* Save the tree list in the block */
    impEndTreeList(block);

    // impEndTreeList sets BBF_IMPORTED on the block
    // We do *NOT* want to set it later than this because
    // impReimportSpillClique might clear it if this block is both a
    // predecessor and successor in the current spill clique
    assert(block->HasFlag(BBF_IMPORTED));

    // If we had a int/native int, or float/double collision, we need to re-import
    if (reimportSpillClique)
    {
        // This will re-import all the successors of block (as well as each of their predecessors)
        impReimportSpillClique(block);

        // We don't expect to see BBJ_EHFILTERRET here.
        assert(!block->KindIs(BBJ_EHFILTERRET));

        for (BasicBlock* const succ : block->Succs())
        {
            if (!succ->HasFlag(BBF_IMPORTED))
            {
                impImportBlockPending(succ);
            }
        }
    }
    else // the normal case
    {
        // otherwise just import the successors of block

        // Does this block jump to any other blocks?
        // Filter successor from BBJ_EHFILTERRET have already been handled above in the call
        // to impVerifyEHBlock().
        if (!block->KindIs(BBJ_EHFILTERRET))
        {
            for (BasicBlock* const succ : block->Succs())
            {
                impImportBlockPending(succ);
            }
        }
    }
}

//------------------------------------------------------------------------
// impImportBlockPending: ensure that block will be imported
//
// Arguments:
//    block - block that should be imported.
//
// Notes:
//   Ensures that "block" is a member of the list of BBs waiting to be imported, pushing it on the list if
//   necessary (and ensures that it is a member of the set of BB's on the list, by setting its byte in
//   impPendingBlockMembers).  Does *NOT* change the existing "pre-state" of the block.
//
//   Merges the current verification state into the verification state of "block" (its "pre-state")./
//
void Compiler::impImportBlockPending(BasicBlock* block)
{
    JITDUMP("\nimpImportBlockPending for " FMT_BB "\n", block->bbNum);

    // We will add a block to the pending set if it has not already been imported (or needs to be re-imported),
    // or if it has, but merging in a predecessor's post-state changes the block's pre-state.
    // (When we're doing verification, we always attempt the merge to detect verification errors.)

    // If the block has not been imported, add to pending set.
    bool addToPending = !block->HasFlag(BBF_IMPORTED);

    // Initialize bbEntryState just the first time we try to add this block to the pending list
    // Just because bbEntryState is NULL, doesn't mean the pre-state wasn't previously set
    // We use NULL to indicate the 'common' state to avoid memory allocation
    if ((block->bbEntryState == nullptr) && !block->HasFlag(BBF_IMPORTED) && (impGetPendingBlockMember(block) == 0))
    {
        initBBEntryState(block, &stackState);
        assert(block->bbStkDepth == 0);
        block->bbStkDepth = static_cast<unsigned short>(stackState.esStackDepth);
        assert(addToPending);
        assert(impGetPendingBlockMember(block) == 0);
    }
    else
    {
        // The stack should have the same height on entry to the block from all its predecessors.
        if (block->bbStkDepth != stackState.esStackDepth)
        {
#ifdef DEBUG
            char buffer[400];
            sprintf_s(buffer, sizeof(buffer),
                      "Block at offset %4.4x to %4.4x in %0.200s entered with different stack depths.\n"
                      "Previous depth was %d, current depth is %d",
                      block->bbCodeOffs, block->bbCodeOffsEnd, info.compFullName, block->bbStkDepth,
                      stackState.esStackDepth);
            buffer[400 - 1] = 0;
            NO_WAY(buffer);
#else
            NO_WAY("Block entered with different stack depths");
#endif
        }

        if (!addToPending)
        {
            return;
        }

        if (block->bbStkDepth > 0)
        {
            // We need to fix the types of any spill temps that might have changed:
            //   int->native int, float->double, int->byref, etc.
            impRetypeEntryStateTemps(block);
        }

        // OK, we must add to the pending list, if it's not already in it.
        if (impGetPendingBlockMember(block) != 0)
        {
            return;
        }
    }

    // Get an entry to add to the pending list

    PendingDsc* dsc;

    if (impPendingFree)
    {
        // We can reuse one of the freed up dscs.
        dsc            = impPendingFree;
        impPendingFree = dsc->pdNext;
    }
    else
    {
        // We have to create a new dsc
        dsc = new (this, CMK_Unknown) PendingDsc;
    }

    dsc->pdBB                 = block;
    dsc->pdSavedStack.ssDepth = stackState.esStackDepth;

    // Save the stack trees for later

    if (stackState.esStackDepth)
    {
        impSaveStackState(&dsc->pdSavedStack, false);
    }

    // Add the entry to the pending list

    dsc->pdNext    = impPendingList;
    impPendingList = dsc;
    impSetPendingBlockMember(block, 1); // And indicate that it's now a member of the set.

    // Various assertions require us to now to consider the block as not imported (at least for
    // the final time...)
    block->RemoveFlags(BBF_IMPORTED);

#ifdef DEBUG
    if (verbose && 0)
    {
        printf("Added PendingDsc - %08p for " FMT_BB "\n", dspPtr(dsc), block->bbNum);
    }
#endif
}

/*****************************************************************************/
//
// Ensures that "block" is a member of the list of BBs waiting to be imported, pushing it on the list if
// necessary (and ensures that it is a member of the set of BB's on the list, by setting its byte in
// impPendingBlockMembers).  Does *NOT* change the existing "pre-state" of the block.

void Compiler::impReimportBlockPending(BasicBlock* block)
{
    JITDUMP("\nimpReimportBlockPending for " FMT_BB, block->bbNum);

    assert(block->HasFlag(BBF_IMPORTED));

    // OK, we must add to the pending list, if it's not already in it.
    if (impGetPendingBlockMember(block) != 0)
    {
        return;
    }

    // Get an entry to add to the pending list

    PendingDsc* dsc;

    if (impPendingFree)
    {
        // We can reuse one of the freed up dscs.
        dsc            = impPendingFree;
        impPendingFree = dsc->pdNext;
    }
    else
    {
        // We have to create a new dsc
        dsc = new (this, CMK_ImpStack) PendingDsc;
    }

    dsc->pdBB = block;

    if (block->bbEntryState)
    {
        dsc->pdSavedStack.ssDepth = block->bbEntryState->esStackDepth;
        dsc->pdSavedStack.ssTrees = block->bbEntryState->esStack;
    }
    else
    {
        dsc->pdSavedStack.ssDepth = 0;
        dsc->pdSavedStack.ssTrees = nullptr;
    }

    // Add the entry to the pending list

    dsc->pdNext    = impPendingList;
    impPendingList = dsc;
    impSetPendingBlockMember(block, 1); // And indicate that it's now a member of the set.

    // Various assertions require us to now to consider the block as not imported (at least for
    // the final time...)
    block->RemoveFlags(BBF_IMPORTED);

#ifdef DEBUG
    if (verbose && 0)
    {
        printf("Added PendingDsc - %08p for " FMT_BB "\n", dspPtr(dsc), block->bbNum);
    }
#endif
}

void* Compiler::BlockListNode::operator new(size_t sz, Compiler* comp)
{
    if (comp->impBlockListNodeFreeList == nullptr)
    {
        return comp->getAllocator(CMK_BasicBlock).allocate<BlockListNode>(1);
    }
    else
    {
        BlockListNode* res             = comp->impBlockListNodeFreeList;
        comp->impBlockListNodeFreeList = res->m_next;
        return res;
    }
}

void Compiler::FreeBlockListNode(Compiler::BlockListNode* node)
{
    node->m_next             = impBlockListNodeFreeList;
    impBlockListNodeFreeList = node;
}

void Compiler::impWalkSpillCliqueFromPred(BasicBlock* block, SpillCliqueWalker* callback)
{
    bool toDo = true;

    BlockListNode* succCliqueToDo = nullptr;
    BlockListNode* predCliqueToDo = new (this) BlockListNode(block);
    while (toDo)
    {
        toDo = false;
        // Look at the successors of every member of the predecessor to-do list.
        while (predCliqueToDo != nullptr)
        {
            BlockListNode* node = predCliqueToDo;
            predCliqueToDo      = node->m_next;
            BasicBlock* blk     = node->m_blk;
            FreeBlockListNode(node);

            for (BasicBlock* const succ : blk->Succs())
            {
                // If it's not already in the clique, add it, and also add it
                // as a member of the successor "toDo" set.
                if (impSpillCliqueGetMember(SpillCliqueSucc, succ) == 0)
                {
                    callback->Visit(SpillCliqueSucc, succ);
                    impSpillCliqueSetMember(SpillCliqueSucc, succ, 1);
                    succCliqueToDo = new (this) BlockListNode(succ, succCliqueToDo);
                    toDo           = true;
                }
            }
        }
        // Look at the predecessors of every member of the successor to-do list.
        while (succCliqueToDo != nullptr)
        {
            BlockListNode* node = succCliqueToDo;
            succCliqueToDo      = node->m_next;
            BasicBlock* blk     = node->m_blk;
            FreeBlockListNode(node);

            for (BasicBlock* predBlock : blk->PredBlocks())
            {
                // If it's not already in the clique, add it, and also add it
                // as a member of the predecessor "toDo" set.
                if (impSpillCliqueGetMember(SpillCliquePred, predBlock) == 0)
                {
                    callback->Visit(SpillCliquePred, predBlock);
                    impSpillCliqueSetMember(SpillCliquePred, predBlock, 1);
                    predCliqueToDo = new (this) BlockListNode(predBlock, predCliqueToDo);
                    toDo           = true;
                }
            }
        }
    }

    // If this fails, it means we didn't walk the spill clique properly and somehow managed
    // miss walking back to include the predecessor we started from.
    // This most likely cause: missing or out of date bbPreds
    assert(impSpillCliqueGetMember(SpillCliquePred, block) != 0);
}

void Compiler::SetSpillTempsBase::Visit(SpillCliqueDir predOrSucc, BasicBlock* blk)
{
    if (predOrSucc == SpillCliqueSucc)
    {
        assert(blk->bbStkTempsIn == NO_BASE_TMP); // Should not already be a member of a clique as a successor.
        blk->bbStkTempsIn = m_baseTmp;
    }
    else
    {
        assert(predOrSucc == SpillCliquePred);
        assert(blk->bbStkTempsOut == NO_BASE_TMP); // Should not already be a member of a clique as a predecessor.
        blk->bbStkTempsOut = m_baseTmp;
    }
}

void Compiler::ReimportSpillClique::Visit(SpillCliqueDir predOrSucc, BasicBlock* blk)
{
    // For Preds we could be a little smarter and just find the existing store
    // and re-type it/add a cast, but that is complicated and hopefully very rare, so
    // just re-import the whole block (just like we do for successors)

    if (!blk->HasFlag(BBF_IMPORTED) && (m_pComp->impGetPendingBlockMember(blk) == 0))
    {
        // If we haven't imported this block (EntryState == NULL) and we're not going to
        // (because it isn't on the pending list) then just ignore it for now.
        assert(blk->bbEntryState == nullptr);
        return;
    }

    // For successors we have a valid stackState, so just mark them for reimport
    // the 'normal' way
    // Unlike predecessors, we *DO* need to reimport the current block because the
    // initial import had the wrong entry state types.
    // Similarly, blocks that are currently on the pending list, still need to call
    // impImportBlockPending to fixup their entry state.
    if (predOrSucc == SpillCliqueSucc)
    {
        m_pComp->impReimportMarkBlock(blk);

        // Set the current stack state to that of the blk->bbEntryState
        m_pComp->resetCurrentState(blk, &m_pComp->stackState);

        m_pComp->impImportBlockPending(blk);
    }
    else if ((blk != m_pComp->compCurBB) && blk->HasFlag(BBF_IMPORTED))
    {
        // As described above, we are only visiting predecessors so they can
        // add the appropriate casts, since we have already done that for the current
        // block, it does not need to be reimported.
        // Nor do we need to reimport blocks that are still pending, but not yet
        // imported.
        //
        // For predecessors, we have no state to seed the EntryState, so we just have
        // to assume the existing one is correct.
        // If the block is also a successor, it will get the EntryState properly
        // updated when it is visited as a successor in the above "if" block.
        assert(predOrSucc == SpillCliquePred);
        m_pComp->impReimportBlockPending(blk);
    }
}

// Re-type the incoming lclVar nodes to match the varDsc.
void Compiler::impRetypeEntryStateTemps(BasicBlock* blk)
{
    if (blk->bbEntryState != nullptr)
    {
        EntryState* es = blk->bbEntryState;
        for (unsigned level = 0; level < es->esStackDepth; level++)
        {
            GenTree* tree = es->esStack[level].val;
            if (tree->OperIs(GT_LCL_VAR) || tree->OperIs(GT_LCL_FLD))
            {
                es->esStack[level].val->gtType = lvaGetDesc(tree->AsLclVarCommon())->TypeGet();
            }
        }
    }
}

unsigned Compiler::impGetSpillTmpBase(BasicBlock* block)
{
    if (block->bbStkTempsOut != NO_BASE_TMP)
    {
        return block->bbStkTempsOut;
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("\n*************** In impGetSpillTmpBase(" FMT_BB ")\n", block->bbNum);
    }
#endif // DEBUG

    // Otherwise, choose one, and propagate to all members of the spill clique.
    // Grab enough temps for the whole stack.
    unsigned          baseTmp = lvaGrabTemps(stackState.esStackDepth DEBUGARG("IL Stack Entries"));
    SetSpillTempsBase callback(baseTmp);

    // We do *NOT* need to reset the SpillClique*Members because a block can only be the predecessor
    // to one spill clique, and similarly can only be the successor to one spill clique
    impWalkSpillCliqueFromPred(block, &callback);

    return baseTmp;
}

void Compiler::impReimportSpillClique(BasicBlock* block)
{
#ifdef DEBUG
    if (verbose)
    {
        printf("\n*************** In impReimportSpillClique(" FMT_BB ")\n", block->bbNum);
    }
#endif // DEBUG

    // If we get here, it is because this block is already part of a spill clique
    // and one predecessor had an outgoing live stack slot of type int, and this
    // block has an outgoing live stack slot of type native int.
    // We need to reset these before traversal because they have already been set
    // by the previous walk to determine all the members of the spill clique.
    impInlineRoot()->impSpillCliquePredMembers.Reset();
    impInlineRoot()->impSpillCliqueSuccMembers.Reset();

    ReimportSpillClique callback(this);

    impWalkSpillCliqueFromPred(block, &callback);
}

// Set the pre-state of "block" (which should not have a pre-state allocated) to
// a copy of "srcState", cloning tree pointers as required.
void Compiler::initBBEntryState(BasicBlock* block, EntryState* srcState)
{
    if (srcState->esStackDepth == 0)
    {
        block->bbEntryState = nullptr;
        return;
    }

    block->bbEntryState = getAllocator(CMK_Unknown).allocate<EntryState>(1);

    // block->bbEntryState.esRefcount = 1;

    block->bbEntryState->esStackDepth = srcState->esStackDepth;

    if (srcState->esStackDepth > 0)
    {
        block->bbSetStack(new (this, CMK_Unknown) StackEntry[srcState->esStackDepth]);
        unsigned stackSize = srcState->esStackDepth * sizeof(StackEntry);

        memcpy(block->bbEntryState->esStack, srcState->esStack, stackSize);
        for (unsigned level = 0; level < srcState->esStackDepth; level++)
        {
            GenTree* tree                           = srcState->esStack[level].val;
            block->bbEntryState->esStack[level].val = gtCloneExpr(tree);
        }
    }
}

/*
 * Resets the current state to the state at the start of the basic block
 */
void Compiler::resetCurrentState(BasicBlock* block, EntryState* destState)
{
    if (block->bbEntryState == nullptr)
    {
        destState->esStackDepth = 0;
        return;
    }

    destState->esStackDepth = block->bbEntryState->esStackDepth;

    if (destState->esStackDepth > 0)
    {
        unsigned stackSize = destState->esStackDepth * sizeof(StackEntry);

        memcpy(destState->esStack, block->bbStackOnEntry(), stackSize);
    }
}

void Compiler::initCurrentState()
{
    // initialize stack info
    stackState.esStackDepth = 0;
    assert(stackState.esStack != nullptr);

    // copy current state to entry state of first BB
    initBBEntryState(fgFirstBB, &stackState);
}

Compiler* Compiler::impInlineRoot()
{
    if (impInlineInfo == nullptr)
    {
        return this;
    }
    else
    {
        return impInlineInfo->InlineRoot;
    }
}

BYTE Compiler::impSpillCliqueGetMember(SpillCliqueDir predOrSucc, BasicBlock* blk)
{
    if (predOrSucc == SpillCliquePred)
    {
        return impInlineRoot()->impSpillCliquePredMembers.Get(blk->bbInd());
    }
    else
    {
        assert(predOrSucc == SpillCliqueSucc);
        return impInlineRoot()->impSpillCliqueSuccMembers.Get(blk->bbInd());
    }
}

void Compiler::impSpillCliqueSetMember(SpillCliqueDir predOrSucc, BasicBlock* blk, BYTE val)
{
    if (predOrSucc == SpillCliquePred)
    {
        impInlineRoot()->impSpillCliquePredMembers.Set(blk->bbInd(), val);
    }
    else
    {
        assert(predOrSucc == SpillCliqueSucc);
        impInlineRoot()->impSpillCliqueSuccMembers.Set(blk->bbInd(), val);
    }
}

//------------------------------------------------------------------------
// impImport: convert IL into jit IR
//
// Notes:
//
// The basic flowgraph has already been constructed. Blocks are filled in
// by the importer as they are discovered to be reachable.
//
// Blocks may be added to provide the right structure for various EH
// constructs (notably LEAVEs from catches and finallies).
//
void Compiler::impImport()
{
    Compiler* const inlineRoot = impInlineRoot();

    if (info.compMaxStack <= SMALL_STACK_SIZE)
    {
        impStkSize = SMALL_STACK_SIZE;
    }
    else
    {
        impStkSize = info.compMaxStack;
    }

    if (this == inlineRoot)
    {
        // Allocate the stack contents
        stackState.esStack = new (this, CMK_ImpStack) StackEntry[impStkSize];
    }
    else
    {
        // This is the inlinee compiler, steal the stack from the inliner compiler
        // (after ensuring that it is large enough).
        if (inlineRoot->impStkSize < impStkSize)
        {
            inlineRoot->impStkSize         = impStkSize;
            inlineRoot->stackState.esStack = new (this, CMK_ImpStack) StackEntry[impStkSize];
        }

        stackState.esStack = inlineRoot->stackState.esStack;
    }

    // initialize the entry state at start of method
    initCurrentState();

    // Initialize stuff related to figuring "spill cliques" (see spec comment for impGetSpillTmpBase).
    if (this == inlineRoot) // These are only used on the root of the inlining tree.
    {
        // We have initialized these previously, but to size 0.  Make them larger.
        impPendingBlockMembers.Init(getAllocator(), fgBBNumMax * 2);
        impSpillCliquePredMembers.Init(getAllocator(), fgBBNumMax * 2);
        impSpillCliqueSuccMembers.Init(getAllocator(), fgBBNumMax * 2);
    }
    inlineRoot->impPendingBlockMembers.Reset(fgBBNumMax * 2);
    inlineRoot->impSpillCliquePredMembers.Reset(fgBBNumMax * 2);
    inlineRoot->impSpillCliqueSuccMembers.Reset(fgBBNumMax * 2);
    impBlockListNodeFreeList = nullptr;

#ifdef DEBUG
    impLastILoffsStmt   = nullptr;
    impNestedStackSpill = false;
#endif
    impBoxTemp = BAD_VAR_NUM;

    impPendingList = impPendingFree = nullptr;

    // Skip leading internal blocks.
    // These can arise from needing a leading scratch BB, from EH normalization, and from OSR entry redirects.
    //
    BasicBlock* entryBlock = fgFirstBB;

    while (entryBlock->HasFlag(BBF_INTERNAL))
    {
        JITDUMP("Marking leading BBF_INTERNAL block " FMT_BB " as BBF_IMPORTED\n", entryBlock->bbNum);
        entryBlock->SetFlags(BBF_IMPORTED);

        assert(entryBlock->KindIs(BBJ_ALWAYS));
        entryBlock = entryBlock->GetTarget();
    }

    // Note for OSR we'd like to be able to verify this block must be
    // stack empty, but won't know that until we've imported...so instead
    // we'll BADCODE out if we mess up.
    //
    // (the concern here is that the runtime asks us to OSR a
    // different IL version than the one that matched the method that
    // triggered OSR).  This should not happen but I might have the
    // IL versioning stuff wrong.
    //
    // TODO: we also currently expect this block to be a join point,
    // which we should verify over when we find jump targets.
    impImportBlockPending(entryBlock);

    if (opts.IsOSR())
    {
        // We now import all the IR and keep it around so we can
        // analyze address exposure more robustly.
        //
        JITDUMP("OSR: protecting original method entry " FMT_BB "\n", fgEntryBB->bbNum);
        impImportBlockPending(fgEntryBB);
        fgEntryBB->bbRefs++;
        fgEntryBBExtraRefs++;
    }

    /* Import blocks in the worker-list until there are no more */

    while (impPendingList)
    {
        /* Remove the entry at the front of the list */

        PendingDsc* dsc = impPendingList;
        impPendingList  = impPendingList->pdNext;
        impSetPendingBlockMember(dsc->pdBB, 0);

        /* Restore the stack state */

        stackState.esStackDepth = dsc->pdSavedStack.ssDepth;
        if (stackState.esStackDepth)
        {
            impRestoreStackState(&dsc->pdSavedStack);
        }

        /* Add the entry to the free list for reuse */

        dsc->pdNext    = impPendingFree;
        impPendingFree = dsc;

        /* Now import the block */
        impImportBlock(dsc->pdBB);

        if (compDonotInline())
        {
            return;
        }
    }

    // If the method had EH, we may be missing some pred edges
    // (notably those from BBJ_EHFINALLYRET blocks). Add them.
    //
    if (info.compXcptnsCount > 0)
    {
        impFixPredLists();
        JITDUMP("\nAfter impImport() added blocks for try,catch,finally");
        JITDUMPEXEC(fgDispBasicBlocks());
    }
}

//------------------------------------------------------------------------
// impFixPredLists: add pred edges from finally returns to their continuations
//
// Notes:
//   These edges were not added during the initial pred list computation,
//   because the initial flow graph does not contain the callfinally
//   block pairs; those blocks are added during importation.
//
//   We rely on handler blocks being lexically contiguous between begin and last.
//
void Compiler::impFixPredLists()
{
    unsigned   XTnum               = 0;
    bool       added               = false;
    const bool usingProfileWeights = fgIsUsingProfileWeights();

    for (EHblkDsc* HBtab = compHndBBtab; XTnum < compHndBBtabCount; XTnum++, HBtab++)
    {
        if (HBtab->HasFinallyHandler())
        {
            BasicBlock* const finallyBegBlock  = HBtab->ebdHndBeg;
            BasicBlock* const finallyLastBlock = HBtab->ebdHndLast;
            unsigned          predCount        = (unsigned)-1;
            const weight_t    finallyWeight    = finallyBegBlock->bbWeight;

            for (BasicBlock* const finallyBlock : BasicBlockRangeList(finallyBegBlock, finallyLastBlock))
            {
                if (finallyBlock->getHndIndex() != XTnum)
                {
                    // Must be a nested handler... we could skip to its last
                    //
                    continue;
                }

                if (!finallyBlock->KindIs(BBJ_EHFINALLYRET))
                {
                    continue;
                }

                // Count the number of predecessors. Then we can allocate the bbEhfTargets table and fill it in.
                // We only need to count once, since it's invariant with the finally block.
                if (predCount == (unsigned)-1)
                {
                    predCount = 0;
                    for (BasicBlock* const predBlock : finallyBegBlock->PredBlocks())
                    {
                        // We only care about preds that are callfinallies.
                        //
                        if (!predBlock->KindIs(BBJ_CALLFINALLY))
                        {
                            continue;
                        }
                        ++predCount;
                    }
                }

                BBJumpTable* jumpEhf;

                if (predCount > 0)
                {
                    FlowEdge** const succTab             = new (this, CMK_FlowEdge) FlowEdge*[predCount];
                    unsigned         predNum             = 0;
                    weight_t         remainingLikelihood = 1.0;
                    for (BasicBlock* const predBlock : finallyBegBlock->PredBlocks())
                    {
                        // We only care about preds that are callfinallies.
                        //
                        if (!predBlock->KindIs(BBJ_CALLFINALLY))
                        {
                            continue;
                        }

                        BasicBlock* const continuation = predBlock->Next();
                        FlowEdge* const   newEdge      = fgAddRefPred(continuation, finallyBlock);

                        if (usingProfileWeights && (finallyWeight != BB_ZERO_WEIGHT))
                        {
                            // Derive edge likelihood from the entry block's weight relative to other entries.
                            //
                            const weight_t callFinallyWeight = predBlock->bbWeight;
                            const weight_t likelihood        = min(callFinallyWeight / finallyWeight, 1.0);
                            newEdge->setLikelihood(min(likelihood, remainingLikelihood));
                            remainingLikelihood = max(BB_ZERO_WEIGHT, remainingLikelihood - likelihood);
                        }
                        else
                        {
                            // If we don't have profile data, evenly distribute the likelihoods.
                            //
                            newEdge->setLikelihood(1.0 / predCount);
                        }

                        succTab[predNum++] = newEdge;

                        if (!added)
                        {
                            JITDUMP("\nAdding pred edges from BBJ_EHFINALLYRET blocks\n");
                            added = true;
                        }
                    }

                    assert(predNum == predCount);
                    jumpEhf = new (this, CMK_FlowEdge) BBJumpTable(succTab, predCount);
                }
                else
                {
                    // It's possible for the `finally` to have no CALLFINALLY predecessors if the `try` block
                    // has an unconditional `throw` (the finally will still be invoked in the exceptional
                    // case via the runtime). In that case, jumpEhf->succCount remains the default, zero,
                    // and jumpEhf->succs remains the default, nullptr.
                    jumpEhf = new (this, CMK_FlowEdge) BBJumpTable();
                }

                finallyBlock->SetEhfTargets(jumpEhf);
            }

            if (usingProfileWeights)
            {
                // Compute new flow into the finally region's continuation successors.
                //
                bool profileConsistent = true;
                for (BasicBlock* const callFinally : finallyBegBlock->PredBlocks())
                {
                    BasicBlock* const callFinallyRet = callFinally->Next();
                    callFinallyRet->setBBProfileWeight(callFinallyRet->computeIncomingWeight());
                    profileConsistent &=
                        fgProfileWeightsConsistentOrSmall(callFinally->bbWeight, callFinallyRet->bbWeight);
                }

                if (!profileConsistent)
                {
                    JITDUMP("Flow into finally handler EH%u does not match outgoing flow. Data %s inconsistent.\n",
                            XTnum, fgPgoConsistent ? "is now" : "was already");
                    fgPgoConsistent = false;
                }
            }
        }
    }
}

//------------------------------------------------------------------------
// impIsInvariant: check if a tree (created during import) is invariant.
//
// Arguments:
//   tree -- The tree
//
// Returns:
//   true if it is invariant
//
// Remarks:
//   This is a variant of GenTree::IsInvariant that is more suitable for use
//   during import. Unlike that function, this one handles GT_FIELD_ADDR nodes.
//
bool Compiler::impIsInvariant(const GenTree* tree)
{
    return tree->OperIsConst() || impIsAddressInLocal(tree) || tree->OperIs(GT_FTN_ADDR);
}

//------------------------------------------------------------------------
// impIsAddressInLocal:
//   Check to see if the tree is the address of a local or
//   the address of a field in a local.
// Arguments:
//     tree -- The tree
//     lclVarTreeOut -- [out] the local that this points into
//
// Returns:
//     true if it points into a local
//
bool Compiler::impIsAddressInLocal(const GenTree* tree, GenTree** lclVarTreeOut)
{
    const GenTree* op = tree;
    while (op->OperIs(GT_FIELD_ADDR) && op->AsFieldAddr()->IsInstance())
    {
        op = op->AsFieldAddr()->GetFldObj();
    }

    if (op->OperIs(GT_LCL_ADDR))
    {
        if (lclVarTreeOut != nullptr)
        {
            *lclVarTreeOut = const_cast<GenTree*>(op);
        }

        return true;
    }

    return false;
}

//------------------------------------------------------------------------
// impMakeDiscretionaryInlineObservations: make observations that help
// determine the profitability of a discretionary inline
//
// Arguments:
//    pInlineInfo -- InlineInfo for the inline, or null for the prejit root
//    inlineResult -- InlineResult accumulating information about this inline
//
// Notes:
//    If inlining or prejitting the root, this method also makes
//    various observations about the method that factor into inline
//    decisions. It sets `compNativeSizeEstimate` as a side effect.

void Compiler::impMakeDiscretionaryInlineObservations(InlineInfo* pInlineInfo, InlineResult* inlineResult)
{
    assert((pInlineInfo != nullptr && compIsForInlining()) || // Perform the actual inlining.
           (pInlineInfo == nullptr && !compIsForInlining())   // Calculate the static inlining hint for AOT.
    );

    // If we're really inlining, we should just have one result in play.
    assert((pInlineInfo == nullptr) || (inlineResult == pInlineInfo->inlineResult));

    // If this is a "forceinline" method, the JIT probably shouldn't have gone
    // to the trouble of estimating the native code size. Even if it did, it
    // shouldn't be relying on the result of this method.
    assert(inlineResult->GetObservation() == InlineObservation::CALLEE_IS_DISCRETIONARY_INLINE);

    // Note if the caller contains NEWOBJ or NEWARR.
    Compiler* rootCompiler = impInlineRoot();

    if ((rootCompiler->optMethodFlags & OMF_HAS_NEWARRAY) != 0)
    {
        inlineResult->Note(InlineObservation::CALLER_HAS_NEWARRAY);
    }

    if ((rootCompiler->optMethodFlags & OMF_HAS_NEWOBJ) != 0)
    {
        inlineResult->Note(InlineObservation::CALLER_HAS_NEWOBJ);
    }

    bool calleeIsStatic  = (info.compFlags & CORINFO_FLG_STATIC) != 0;
    bool isSpecialMethod = (info.compFlags & CORINFO_FLG_CONSTRUCTOR) != 0;

    if (isSpecialMethod)
    {
        if (calleeIsStatic)
        {
            inlineResult->Note(InlineObservation::CALLEE_IS_CLASS_CTOR);
        }
        else
        {
            inlineResult->Note(InlineObservation::CALLEE_IS_INSTANCE_CTOR);
        }
    }
    else if (!calleeIsStatic)
    {
        // Callee is an instance method.
        //
        // Check if the callee has the same 'this' as the root.
        if (pInlineInfo != nullptr)
        {
            GenTree* thisArg = pInlineInfo->iciCall->AsCall()->gtArgs.GetThisArg()->GetNode();
            assert(thisArg);
            bool isSameThis = impIsThis(thisArg);
            inlineResult->NoteBool(InlineObservation::CALLSITE_IS_SAME_THIS, isSameThis);
        }
    }

    bool callsiteIsGeneric = (rootCompiler->info.compMethodInfo->args.sigInst.methInstCount != 0) ||
                             (rootCompiler->info.compMethodInfo->args.sigInst.classInstCount != 0);

    bool calleeIsGeneric = (info.compMethodInfo->args.sigInst.methInstCount != 0) ||
                           (info.compMethodInfo->args.sigInst.classInstCount != 0);

    if (!callsiteIsGeneric && calleeIsGeneric)
    {
        inlineResult->Note(InlineObservation::CALLSITE_NONGENERIC_CALLS_GENERIC);
    }

    // Inspect callee's arguments (and the actual values at the callsite for them)
    CORINFO_SIG_INFO        sig    = info.compMethodInfo->args;
    CORINFO_ARG_LIST_HANDLE sigArg = sig.args;

    CallArg* argUse = pInlineInfo == nullptr ? nullptr : pInlineInfo->iciCall->AsCall()->gtArgs.Args().begin().GetArg();

    for (unsigned i = 0; i < info.compMethodInfo->args.numArgs; i++)
    {
        if ((argUse != nullptr) && (argUse->GetWellKnownArg() == WellKnownArg::ThisPointer))
        {
            argUse = argUse->GetNext();
        }

        CORINFO_CLASS_HANDLE sigClass;
        CorInfoType          corType = strip(info.compCompHnd->getArgType(&sig, sigArg, &sigClass));
        GenTree*             argNode = argUse == nullptr ? nullptr : argUse->GetEarlyNode();

        if (corType == CORINFO_TYPE_CLASS)
        {
            sigClass = info.compCompHnd->getArgClass(&sig, sigArg);
        }
        else if (corType == CORINFO_TYPE_VALUECLASS)
        {
            inlineResult->Note(InlineObservation::CALLEE_ARG_STRUCT);
        }
        else if (corType == CORINFO_TYPE_BYREF)
        {
            sigClass = info.compCompHnd->getArgClass(&sig, sigArg);
            corType  = info.compCompHnd->getChildType(sigClass, &sigClass);
        }

        if (argNode != nullptr)
        {
            bool                 isExact   = false;
            bool                 isNonNull = false;
            CORINFO_CLASS_HANDLE argCls    = gtGetClassHandle(argNode, &isExact, &isNonNull);
            if (argCls != nullptr)
            {
                const bool isArgValueType = eeIsValueClass(argCls);
                // Exact class of the arg is known
                if (isExact && !isArgValueType)
                {
                    inlineResult->Note(InlineObservation::CALLSITE_ARG_EXACT_CLS);
                    if ((argCls != sigClass) && (sigClass != nullptr))
                    {
                        // .. but the signature accepts a less concrete type.
                        inlineResult->Note(InlineObservation::CALLSITE_ARG_EXACT_CLS_SIG_IS_NOT);
                    }
                }
                // Arg is a reference type in the signature and a boxed value type was passed.
                else if (isArgValueType && (corType == CORINFO_TYPE_CLASS))
                {
                    inlineResult->Note(InlineObservation::CALLSITE_ARG_BOXED);
                }
            }

            if (argNode->OperIsConst())
            {
                inlineResult->Note(InlineObservation::CALLSITE_ARG_CONST);
            }
            argUse = argUse->GetNext();
        }
        sigArg = info.compCompHnd->getArgNext(sigArg);
    }

    // Note if the callee's return type is a value type
    if (info.compMethodInfo->args.retType == CORINFO_TYPE_VALUECLASS)
    {
        inlineResult->Note(InlineObservation::CALLEE_RETURNS_STRUCT);
    }

    // Note if the callee's class is a promotable struct
    if ((info.compClassAttr & CORINFO_FLG_VALUECLASS) != 0)
    {
        assert(structPromotionHelper != nullptr);
        if (structPromotionHelper->CanPromoteStructType(info.compClassHnd))
        {
            inlineResult->Note(InlineObservation::CALLEE_CLASS_PROMOTABLE);
        }
        inlineResult->Note(InlineObservation::CALLEE_CLASS_VALUETYPE);
    }

#ifdef FEATURE_SIMD

    // Note if this method is has SIMD args or return value
    if (pInlineInfo != nullptr && pInlineInfo->hasSIMDTypeArgLocalOrReturn)
    {
        inlineResult->Note(InlineObservation::CALLEE_HAS_SIMD);
    }

#endif // FEATURE_SIMD

    // Roughly classify callsite frequency.
    InlineCallsiteFrequency frequency = InlineCallsiteFrequency::UNUSED;

    // If this is a prejit root, or a maximally hot block...
    if ((pInlineInfo == nullptr) || (pInlineInfo->iciBlock->isMaxBBWeight()))
    {
        frequency = InlineCallsiteFrequency::HOT;
    }
    // No training data.  Look for loop-like things.
    // We consider a recursive call loop-like.  Do not give the inlining boost to the method itself.
    // However, give it to things nearby.
    else if (pInlineInfo->iciBlock->HasFlag(BBF_BACKWARD_JUMP) &&
             (pInlineInfo->fncHandle != pInlineInfo->inlineCandidateInfo->ilCallerHandle))
    {
        frequency = InlineCallsiteFrequency::LOOP;
    }
    else if (pInlineInfo->iciBlock->hasProfileWeight() && (pInlineInfo->iciBlock->bbWeight > BB_ZERO_WEIGHT))
    {
        frequency = InlineCallsiteFrequency::WARM;
    }
    // Now modify the multiplier based on where we're called from.
    else if (pInlineInfo->iciBlock->isRunRarely() || ((info.compFlags & FLG_CCTOR) == FLG_CCTOR))
    {
        frequency = InlineCallsiteFrequency::RARE;
    }
    else
    {
        frequency = InlineCallsiteFrequency::BORING;
    }

    // Also capture the block weight of the call site.
    //
    // In the prejit root case, assume at runtime there might be a hot call site
    // for this method, so we won't prematurely conclude this method should never
    // be inlined.
    //
    weight_t weight = 0;

    if (pInlineInfo != nullptr)
    {
        weight = pInlineInfo->iciBlock->bbWeight;
    }
    else
    {
        const weight_t prejitHotCallerWeight = 1000000.0;
        weight                               = prejitHotCallerWeight;
    }

    inlineResult->NoteInt(InlineObservation::CALLSITE_FREQUENCY, static_cast<int>(frequency));
    inlineResult->NoteInt(InlineObservation::CALLSITE_WEIGHT, (int)(weight));

    bool   hasProfile  = false;
    double profileFreq = 0.0;

    // If the call site has profile data, report the relative frequency of the site.
    //
    if ((pInlineInfo != nullptr) && rootCompiler->fgHaveSufficientProfileWeights())
    {
        const weight_t callSiteWeight = pInlineInfo->iciBlock->bbWeight;
        const weight_t entryWeight    = rootCompiler->fgCalledCount;
        profileFreq                   = fgProfileWeightsEqual(entryWeight, 0.0) ? 0.0 : callSiteWeight / entryWeight;
        hasProfile                    = true;

        assert(callSiteWeight >= 0);
        assert(entryWeight >= 0);
    }
    else if (pInlineInfo == nullptr)
    {
        // Simulate a hot callsite for PrejitRoot mode.
        hasProfile  = true;
        profileFreq = 1.0;
    }

    inlineResult->NoteBool(InlineObservation::CALLSITE_HAS_PROFILE_WEIGHTS, hasProfile);
    inlineResult->NoteDouble(InlineObservation::CALLSITE_PROFILE_FREQUENCY, profileFreq);
}

//------------------------------------------------------------------------
// impCanInlineIL: screen inline candate based on info from the method header
//
// Arguments:
//   fncHandle -- inline candidate method
//   methInfo -- method info from VM
//   forceInline -- true if method is marked with AggressiveInlining
//   inlineResult -- ongoing inline evaluation
//
void Compiler::impCanInlineIL(CORINFO_METHOD_HANDLE fncHandle,
                              CORINFO_METHOD_INFO*  methInfo,
                              bool                  forceInline,
                              InlineResult*         inlineResult)
{
    unsigned codeSize = methInfo->ILCodeSize;

    // We shouldn't have made up our minds yet...
    assert(!inlineResult->IsDecided());

    if (methInfo->EHcount > 0)
    {
        if (!opts.compInlineMethodsWithEH)
        {
            inlineResult->NoteFatal(InlineObservation::CALLEE_HAS_EH);
            return;
        }
    }

    if ((methInfo->ILCode == nullptr) || (codeSize == 0))
    {
        inlineResult->NoteFatal(InlineObservation::CALLEE_HAS_NO_BODY);
        return;
    }

    // For now we don't inline varargs (import code can't handle it)

    if (methInfo->args.isVarArg())
    {
        inlineResult->NoteFatal(InlineObservation::CALLEE_HAS_MANAGED_VARARGS);
        return;
    }

    // Reject if it has too many locals.
    // This is currently an implementation limit due to fixed-size arrays in the
    // inline info, rather than a performance heuristic.

    inlineResult->NoteInt(InlineObservation::CALLEE_NUMBER_OF_LOCALS, methInfo->locals.numArgs);

    if (methInfo->locals.numArgs > MAX_INL_LCLS)
    {
        inlineResult->NoteFatal(InlineObservation::CALLEE_TOO_MANY_LOCALS);
        return;
    }

    // Make sure there aren't too many arguments.
    // This is currently an implementation limit due to fixed-size arrays in the
    // inline info, rather than a performance heuristic.

    inlineResult->NoteInt(InlineObservation::CALLEE_NUMBER_OF_ARGUMENTS, methInfo->args.numArgs);

    if (methInfo->args.numArgs > MAX_INL_ARGS)
    {
        inlineResult->NoteFatal(InlineObservation::CALLEE_TOO_MANY_ARGUMENTS);
        return;
    }

    // Note force inline state

    inlineResult->NoteBool(InlineObservation::CALLEE_IS_FORCE_INLINE, forceInline);

    // Note IL code size

    inlineResult->NoteInt(InlineObservation::CALLEE_IL_CODE_SIZE, codeSize);

    if (inlineResult->IsFailure())
    {
        return;
    }

    // Make sure maxstack is not too big

    inlineResult->NoteInt(InlineObservation::CALLEE_MAXSTACK, methInfo->maxStack);

    if (inlineResult->IsFailure())
    {
        return;
    }
}

//------------------------------------------------------------------------
// impInlineRecordArgInfo: record information about an inline candidate argument
//
// Arguments:
//   pInlineInfo - inline info for the inline candidate
//   arg - the caller argument
//   argInfo - Structure to record information into
//   inlineResult - result of ongoing inline evaluation
//
// Notes:
//
//   Checks for various inline blocking conditions and makes notes in
//   the inline info arg table about the properties of the actual. These
//   properties are used later by impInlineFetchArg to determine how best to
//   pass the argument into the inlinee.

void Compiler::impInlineRecordArgInfo(InlineInfo*   pInlineInfo,
                                      CallArg*      arg,
                                      InlArgInfo*   argInfo,
                                      InlineResult* inlineResult)
{
    argInfo->arg       = arg;
    GenTree* curArgVal = arg->GetNode();

    assert(!curArgVal->OperIs(GT_RET_EXPR));

    GenTree*   lclVarTree;
    const bool isAddressInLocal = impIsAddressInLocal(curArgVal, &lclVarTree);
    if (isAddressInLocal)
    {
        LclVarDsc* varDsc = lvaGetDesc(lclVarTree->AsLclVarCommon());

        if (varTypeIsStruct(varDsc))
        {
            argInfo->argIsByRefToStructLocal = true;
#ifdef FEATURE_SIMD
            if (varTypeIsSIMD(varDsc))
            {
                pInlineInfo->hasSIMDTypeArgLocalOrReturn = true;
            }
#endif // FEATURE_SIMD
        }

        // Spilling code relies on correct aliasability annotations.
        assert(varDsc->lvHasLdAddrOp || varDsc->IsAddressExposed());
    }

    if (curArgVal->gtFlags & GTF_ALL_EFFECT)
    {
        argInfo->argHasGlobRef = (curArgVal->gtFlags & GTF_GLOB_REF) != 0;
        argInfo->argHasSideEff = (curArgVal->gtFlags & (GTF_ALL_EFFECT & ~GTF_GLOB_REF)) != 0;
    }

    if (curArgVal->OperIs(GT_LCL_VAR))
    {
        argInfo->argIsLclVar = true;
    }

    argInfo->argIsThis = arg->GetWellKnownArg() == WellKnownArg::ThisPointer;

    if (impIsInvariant(curArgVal))
    {
        argInfo->argIsInvariant = true;
        if (argInfo->argIsThis && curArgVal->OperIs(GT_CNS_INT) && (curArgVal->AsIntCon()->gtIconVal == 0))
        {
            // Abort inlining at this call site
            inlineResult->NoteFatal(InlineObservation::CALLSITE_ARG_HAS_NULL_THIS);
            return;
        }
    }
    else if (gtIsTypeof(curArgVal))
    {
        argInfo->argIsInvariant = true;
        argInfo->argHasSideEff  = false;
    }

    bool isExact        = false;
    bool isNonNull      = false;
    argInfo->argIsExact = (gtGetClassHandle(curArgVal, &isExact, &isNonNull) != NO_CLASS_HANDLE) && isExact;

    // If the arg is a local that is address-taken, we can't safely
    // directly substitute it into the inlinee.
    //
    // Previously we'd accomplish this by setting "argHasLdargaOp" but
    // that has a stronger meaning: that the arg value can change in
    // the method body. Using that flag prevents type propagation,
    // which is safe in this case.
    //
    // Instead mark the arg as having a caller local ref.
    if (!argInfo->argIsInvariant && gtHasLocalsWithAddrOp(curArgVal))
    {
        argInfo->argHasCallerLocalRef = true;
    }

#ifdef DEBUG
    if (verbose)
    {
        if (arg->GetWellKnownArg() != WellKnownArg::None)
        {
            printf("%s:", getWellKnownArgName(arg->GetWellKnownArg()));
        }
        else
        {
            printf("IL argument #%u:", pInlineInfo->iciCall->gtArgs.GetUserIndex(arg));
        }
        if (argInfo->argIsLclVar)
        {
            printf(" is a local var");
        }
        if (argInfo->argIsInvariant)
        {
            printf(" is a constant or invariant");
        }
        if (argInfo->argHasGlobRef)
        {
            printf(" has global refs");
        }
        if (argInfo->argHasCallerLocalRef)
        {
            printf(" has caller local ref");
        }
        if (argInfo->argHasSideEff)
        {
            printf(" has side effects");
        }
        if (argInfo->argHasLdargaOp)
        {
            printf(" has ldarga effect");
        }
        if (argInfo->argHasStargOp)
        {
            printf(" has starg effect");
        }
        if (argInfo->argIsByRefToStructLocal)
        {
            printf(" is byref to a struct local");
        }

        printf("\n");
        gtDispTree(curArgVal);
        printf("\n");
    }
#endif
}

//------------------------------------------------------------------------
// impInlineInitVars: setup inline information for inlinee args and locals
//
// Arguments:
//    pInlineInfo - inline info for the inline candidate
//
// Notes:
//    This method primarily adds caller-supplied info to the inlArgInfo
//    and sets up the lclVarInfo table.
//
//    For args, the inlArgInfo records properties of the actual argument
//    including the tree node that produces the arg value. This node is
//    usually the tree node present at the call, but may also differ in
//    various ways:
//    - when the call arg is a GT_RET_EXPR, we search back through the ret
//      expr chain for the actual node. Note this will either be the original
//      call (which will be a failed inline by this point), or the return
//      expression from some set of inlines.
//    - when argument type casting is needed the necessary casts are added
//      around the argument node.
//    - if an argument can be simplified by folding then the node here is the
//      folded value.
//
//   The method may make observations that lead to marking this candidate as
//   a failed inline. If this happens the initialization is abandoned immediately
//   to try and reduce the jit time cost for a failed inline.
//
void Compiler::impInlineInitVars(InlineInfo* pInlineInfo)
{
    assert(!compIsForInlining());

    GenTreeCall*         call         = pInlineInfo->iciCall;
    CORINFO_METHOD_INFO* methInfo     = &pInlineInfo->inlineCandidateInfo->methInfo;
    unsigned             clsAttr      = pInlineInfo->inlineCandidateInfo->clsAttr;
    InlArgInfo*          inlArgInfo   = pInlineInfo->inlArgInfo;
    InlLclVarInfo*       lclVarInfo   = pInlineInfo->lclVarInfo;
    InlineResult*        inlineResult = pInlineInfo->inlineResult;

    /* init the argument struct */
    memset(inlArgInfo, 0, (MAX_INL_ARGS + 1) * sizeof(inlArgInfo[0]));

    unsigned ilArgCnt = 0;
    for (CallArg& arg : call->gtArgs.Args())
    {
        InlArgInfo* argInfo;
        switch (arg.GetWellKnownArg())
        {
            case WellKnownArg::RetBuffer:
            case WellKnownArg::AsyncContinuation:
                // These do not appear in the table of inline arg info; do not include them
                continue;
            case WellKnownArg::InstParam:
                pInlineInfo->inlInstParamArgInfo = argInfo = new (this, CMK_Inlining) InlArgInfo{};
                break;
            default:
                argInfo = &inlArgInfo[ilArgCnt++];
                break;
        }

        arg.SetEarlyNode(gtFoldExpr(arg.GetEarlyNode()));
        impInlineRecordArgInfo(pInlineInfo, &arg, argInfo, inlineResult);

        if (inlineResult->IsFailure())
        {
            return;
        }
    }

#ifdef FEATURE_SIMD
    bool foundSIMDType = pInlineInfo->hasSIMDTypeArgLocalOrReturn;
#endif // FEATURE_SIMD

    /* We have typeless opcodes, get type information from the signature */

    CallArg* thisArg = call->gtArgs.GetThisArg();
    if (thisArg != nullptr)
    {
        bool                 isValueClassThis = ((clsAttr & CORINFO_FLG_VALUECLASS) != 0);
        var_types            sigType          = isValueClassThis ? TYP_BYREF : TYP_REF;
        CORINFO_CLASS_HANDLE sigThisClass     = pInlineInfo->inlineCandidateInfo->clsHandle;

        lclVarInfo[0].lclTypeInfo    = sigType;
        lclVarInfo[0].lclTypeHandle  = isValueClassThis ? NO_CLASS_HANDLE : sigThisClass;
        lclVarInfo[0].lclHasLdlocaOp = false;

#ifdef FEATURE_SIMD
        // We always want to check isSIMDClass, since we want to set foundSIMDType (to increase
        // the inlining multiplier) for anything in that assembly.
        // But we only need to normalize it if it is a TYP_STRUCT
        // (which we need to do even if we have already set foundSIMDType).
        if (!foundSIMDType && isValueClassThis && isSIMDorHWSIMDClass(sigThisClass))
        {
            foundSIMDType = true;
        }
#endif // FEATURE_SIMD

        GenTree* thisArgNode = thisArg->GetEarlyNode();

        assert(varTypeIsGC(thisArgNode->TypeGet()) || // "this" is managed
               (thisArgNode->TypeIs(TYP_I_IMPL) &&    // "this" is unmgd but the method's class doesnt care
                isValueClassThis));

        if (genActualType(thisArgNode) != genActualType(sigType))
        {
            if (sigType == TYP_REF)
            {
                /* The argument cannot be bashed into a ref (see bug 750871) */
                inlineResult->NoteFatal(InlineObservation::CALLSITE_ARG_NO_BASH_TO_REF);
                return;
            }

            /* This can only happen with byrefs <-> ints/shorts */

            assert(sigType == TYP_BYREF);
            assert((genActualType(thisArgNode) == TYP_I_IMPL) || thisArgNode->TypeIs(TYP_BYREF));
        }
    }

    /* Init the types of the arguments and make sure the types
     * from the trees match the types in the signature */

    CORINFO_ARG_LIST_HANDLE argLst;
    argLst = methInfo->args.args;

    // TODO-ARGS: We can presumably just use type info stored in CallArgs
    // instead of reiterating the signature.
    unsigned i;
    for (i = (thisArg ? 1 : 0); i < ilArgCnt; i++, argLst = info.compCompHnd->getArgNext(argLst))
    {
        CORINFO_CLASS_HANDLE argSigClass;
        CorInfoType          argSigJitType = strip(info.compCompHnd->getArgType(&methInfo->args, argLst, &argSigClass));
        var_types            sigType       = TypeHandleToVarType(argSigJitType, argSigClass);

#ifdef FEATURE_SIMD
        // If this is a SIMD class (i.e. in the SIMD assembly), then we will consider that we've
        // found a SIMD type, even if this may not be a type we recognize (the assumption is that
        // it is likely to use a SIMD type, and therefore we want to increase the inlining multiplier).
        if (!foundSIMDType && varTypeIsStruct(sigType) && isSIMDorHWSIMDClass(argSigClass))
        {
            foundSIMDType = true;
        }
#endif // FEATURE_SIMD

        lclVarInfo[i].lclHasLdlocaOp = false;
        lclVarInfo[i].lclTypeInfo    = sigType;
        if (sigType == TYP_REF)
        {
            lclVarInfo[i].lclTypeHandle = eeGetArgClass(&methInfo->args, argLst);
        }
        else if (varTypeIsStruct(sigType))
        {
            lclVarInfo[i].lclTypeHandle = argSigClass;
        }

        // Does the tree type match the signature type?

        GenTree* inlArgNode = inlArgInfo[i].arg->GetNode();

        if (sigType == inlArgNode->gtType)
        {
            continue;
        }

        assert(impCheckImplicitArgumentCoercion(sigType, inlArgNode->gtType));
        assert(!varTypeIsStruct(inlArgNode->gtType) && !varTypeIsStruct(sigType));

        // In valid IL, this can only happen for short integer types or byrefs <-> [native] ints,
        // but in bad IL cases with caller-callee signature mismatches we can see other types.
        // Intentionally reject cases with mismatches so the jit is more flexible when
        // encountering bad IL.

        bool isPlausibleTypeMatch = (genActualType(sigType) == genActualType(inlArgNode->gtType)) ||
                                    (genActualTypeIsIntOrI(sigType) && inlArgNode->TypeIs(TYP_BYREF)) ||
                                    (sigType == TYP_BYREF && genActualTypeIsIntOrI(inlArgNode->gtType));

        if (!isPlausibleTypeMatch)
        {
            inlineResult->NoteFatal(InlineObservation::CALLSITE_ARG_TYPES_INCOMPATIBLE);
            return;
        }

        // The same size but different type of the arguments.
        GenTree** pInlArgNode = &inlArgInfo[i].arg->EarlyNodeRef();

        // Is it a narrowing or widening cast?
        // Widening casts are ok since the value computed is already
        // normalized to an int (on the IL stack)
        if (genTypeSize(inlArgNode) >= genTypeSize(sigType))
        {
            if ((sigType != TYP_BYREF) && inlArgNode->TypeIs(TYP_BYREF))
            {
                assert(varTypeIsIntOrI(sigType));

                /* If possible bash the BYREF to an int */
                if (inlArgNode->OperIs(GT_LCL_ADDR))
                {
                    inlArgNode->gtType = TYP_I_IMPL;
                }
                else
                {
                    // Arguments 'int <- byref' cannot be changed
                    inlineResult->NoteFatal(InlineObservation::CALLSITE_ARG_NO_BASH_TO_INT);
                    return;
                }
            }
            else if (genTypeSize(sigType) < TARGET_POINTER_SIZE)
            {
                // Narrowing cast.
                if (inlArgNode->OperIs(GT_LCL_VAR))
                {
                    const unsigned lclNum = inlArgNode->AsLclVarCommon()->GetLclNum();
                    if (!lvaTable[lclNum].lvNormalizeOnLoad() && sigType == lvaGetRealType(lclNum))
                    {
                        // We don't need to insert a cast here as the variable
                        // was assigned a normalized value of the right type.
                        continue;
                    }
                }

                inlArgNode = gtNewCastNode(TYP_INT, inlArgNode, false, sigType);

                inlArgInfo[i].argIsLclVar = false;
                // Try to fold the node in case we have constant arguments.
                if (inlArgInfo[i].argIsInvariant)
                {
                    inlArgNode = gtFoldExpr(inlArgNode);
                }
                *pInlArgNode = inlArgNode;
            }
#ifdef TARGET_64BIT
            else if (genTypeSize(genActualType(inlArgNode->gtType)) < genTypeSize(sigType))
            {
                // This should only happen for int -> native int widening
                inlArgNode = gtNewCastNode(genActualType(sigType), inlArgNode, false, sigType);

                inlArgInfo[i].argIsLclVar = false;

                // Try to fold the node in case we have constant arguments.
                if (inlArgInfo[i].argIsInvariant)
                {
                    inlArgNode = gtFoldExpr(inlArgNode);
                }
                *pInlArgNode = inlArgNode;
            }
#endif // TARGET_64BIT
        }
    }

    /* Init the types of the local variables */

    CORINFO_ARG_LIST_HANDLE localsSig;
    localsSig = methInfo->locals.args;

    for (i = 0; i < methInfo->locals.numArgs; i++)
    {
        CORINFO_CLASS_HANDLE sigClass;
        ClassLayout*         layout;
        CorInfoTypeWithMod   sigJitTypeWithMod = info.compCompHnd->getArgType(&methInfo->locals, localsSig, &sigClass);
        var_types            type              = TypeHandleToVarType(strip(sigJitTypeWithMod), sigClass, &layout);
        bool                 isPinned          = (sigJitTypeWithMod & ~CORINFO_TYPE_MASK) != 0;

        lclVarInfo[i + ilArgCnt].lclHasLdlocaOp = false;
        lclVarInfo[i + ilArgCnt].lclTypeInfo    = type;
        if (type == TYP_REF)
        {
            lclVarInfo[i + ilArgCnt].lclTypeHandle = eeGetArgClass(&methInfo->locals, localsSig);
        }
        else if (varTypeIsStruct(type))
        {
            lclVarInfo[i + ilArgCnt].lclTypeHandle = sigClass;
        }

        if (varTypeIsGC(type))
        {
            if (isPinned)
            {
                JITDUMP("Inlinee local #%02u is pinned\n", i);
                lclVarInfo[i + ilArgCnt].lclIsPinned = true;

                // Pinned locals may cause inlines to fail.
                inlineResult->Note(InlineObservation::CALLEE_HAS_PINNED_LOCALS);
                if (inlineResult->IsFailure())
                {
                    return;
                }
            }

            pInlineInfo->numberOfGcRefLocals++;
        }
        else if (isPinned)
        {
            JITDUMP("Ignoring pin on inlinee local #%02u -- not a GC type\n", i);
        }

        // If this local is a struct type with GC fields, inform the inliner. It may choose to bail
        // out on the inline.
        if ((type == TYP_STRUCT) && layout->HasGCPtr())
        {
            inlineResult->Note(InlineObservation::CALLEE_HAS_GC_STRUCT);
            if (inlineResult->IsFailure())
            {
                return;
            }

            // Do further notification in the case where the call site is rare; some policies do
            // not track the relative hotness of call sites for "always" inline cases.
            if (pInlineInfo->iciBlock->isRunRarely())
            {
                inlineResult->Note(InlineObservation::CALLSITE_RARE_GC_STRUCT);
                if (inlineResult->IsFailure())
                {
                    return;
                }
            }
        }

#ifdef FEATURE_SIMD
        if (!foundSIMDType && varTypeIsStruct(type) && isSIMDorHWSIMDClass(sigClass))
        {
            foundSIMDType = true;
        }
#endif // FEATURE_SIMD

        localsSig = info.compCompHnd->getArgNext(localsSig);
    }

#ifdef FEATURE_SIMD
    if (!foundSIMDType && (call->AsCall()->gtRetClsHnd != nullptr) && isSIMDorHWSIMDClass(call->AsCall()->gtRetClsHnd))
    {
        foundSIMDType = true;
    }

    pInlineInfo->hasSIMDTypeArgLocalOrReturn = foundSIMDType;
#endif // FEATURE_SIMD
}

//------------------------------------------------------------------------
// impInlineFetchLocal: get a local var that represents an inlinee local
//
// Arguments:
//    lclNum -- number of the inlinee local
//    reason -- debug string describing purpose of the local var
//
// Returns:
//    Number of the local to use
//
// Notes:
//    This method is invoked only for locals actually used in the
//    inlinee body.
//
//    Allocates a new temp if necessary, and copies key properties
//    over from the inlinee local var info.

unsigned Compiler::impInlineFetchLocal(unsigned lclNum DEBUGARG(const char* reason))
{
    assert(compIsForInlining());

    unsigned tmpNum = impInlineInfo->lclTmpNum[lclNum];

    if (tmpNum == BAD_VAR_NUM)
    {
        const InlLclVarInfo& inlineeLocal = impInlineInfo->lclVarInfo[lclNum + impInlineInfo->argCnt];
        const var_types      lclTyp       = inlineeLocal.lclTypeInfo;

        // The lifetime of this local might span multiple BBs.
        // So it is a long lifetime local.
        impInlineInfo->lclTmpNum[lclNum] = tmpNum = lvaGrabTemp(false DEBUGARG(reason));

        // Copy over key info
        lvaTable[tmpNum].lvType                 = lclTyp;
        lvaTable[tmpNum].lvHasLdAddrOp          = inlineeLocal.lclHasLdlocaOp;
        lvaTable[tmpNum].lvPinned               = inlineeLocal.lclIsPinned;
        lvaTable[tmpNum].lvHasILStoreOp         = inlineeLocal.lclHasStlocOp;
        lvaTable[tmpNum].lvHasMultipleILStoreOp = inlineeLocal.lclHasMultipleStlocOp;

        assert(lvaTable[tmpNum].lvSingleDef == 0);

        lvaTable[tmpNum].lvSingleDef = !inlineeLocal.lclHasMultipleStlocOp && !inlineeLocal.lclHasLdlocaOp;
        if (lvaTable[tmpNum].lvSingleDef)
        {
            JITDUMP("Marked V%02u as a single def temp\n", tmpNum);
        }

        // Copy over class handle for ref types. Note this may be a
        // shared type -- someday perhaps we can get the exact
        // signature and pass in a more precise type.
        if (lclTyp == TYP_REF)
        {
            lvaSetClass(tmpNum, inlineeLocal.lclTypeHandle);
        }

        if (varTypeIsStruct(lclTyp))
        {
            lvaSetStruct(tmpNum, inlineeLocal.lclTypeHandle, true /* unsafe value cls check */);
        }

#ifdef DEBUG
        // Sanity check that we're properly prepared for gc ref locals.
        if (varTypeIsGC(lclTyp))
        {
            // Since there are gc locals we should have seen them earlier
            // and if there was a return value, set up the spill temp.
            assert(impInlineInfo->HasGcRefLocals());
            assert((info.compRetNativeType == TYP_VOID) || fgNeedReturnSpillTemp());
        }
        else
        {
            // Make sure all pinned locals count as gc refs.
            assert(!inlineeLocal.lclIsPinned);
        }
#endif // DEBUG
    }

    return tmpNum;
}

//------------------------------------------------------------------------
// impInlineFetchArg: return tree node for argument value in an inlinee
//
// Arguments:
//    argInfo -- argument info for inlinee
//    lclInfo -- var info for inlinee
//
// Returns:
//    Tree for the argument's value. Often an inlinee-scoped temp
//    GT_LCL_VAR but can be other tree kinds, if the argument
//    expression from the caller can be directly substituted into the
//    inlinee body.
//
// Notes:
//    Must be used only for arguments -- use impInlineFetchLocal for
//    inlinee locals.
//
//    Direct substitution is performed when the formal argument cannot
//    change value in the inlinee body (no starg or ldarga), and the
//    actual argument expression's value cannot be changed if it is
//    substituted it into the inlinee body.
//
//    Even if an inlinee-scoped temp is returned here, it may later be
//    "bashed" to a caller-supplied tree when arguments are actually
//    passed (see fgInlinePrependStatements). Bashing can happen if
//    the argument ends up being single use and other conditions are
//    met. So the contents of the tree returned here may not end up
//    being the ones ultimately used for the argument.
//
//    This method will side effect inlArgInfo. It should only be called
//    for actual uses of the argument in the inlinee.
//
GenTree* Compiler::impInlineFetchArg(InlArgInfo& argInfo, const InlLclVarInfo& lclInfo)
{
    // Cache the relevant arg and lcl info for this argument.
    // We will modify argInfo but not lclVarInfo.
    const bool      argCanBeModified = argInfo.argHasLdargaOp || argInfo.argHasStargOp;
    const var_types lclTyp           = lclInfo.lclTypeInfo;
    GenTree*        op1              = nullptr;

    GenTree* argNode = argInfo.arg->GetNode();
    assert(!argNode->OperIs(GT_RET_EXPR));

    // For TYP_REF args, if the argNode doesn't have any class information
    // we will lose some type info if we directly substitute it.
    // We can at least rely on the declared type of the arg here.
    //
    bool argLosesTypeInfo = false;
    if (argNode->TypeIs(TYP_REF))
    {
        bool                 isExact;
        bool                 isNeverNull;
        CORINFO_CLASS_HANDLE argClass = gtGetClassHandle(argNode, &isExact, &isNeverNull);

        argLosesTypeInfo = (argClass == NO_CLASS_HANDLE);
    }

    if (argInfo.argIsInvariant && !argCanBeModified)
    {
        // Directly substitute constants or addresses of locals
        //
        // Clone the constant. Note that we cannot directly use
        // argNode in the trees even if !argInfo.argIsUsed as this
        // would introduce aliasing between inlArgInfo[].argNode and
        // impInlineExpr. Then gtFoldExpr() could change it, causing
        // further references to the argument working off of the
        // bashed copy.
        op1 = gtCloneExpr(argNode);
        assert(op1 != nullptr);
        argInfo.argTmpNum = BAD_VAR_NUM;

        // We may need to retype to ensure we match the callee's view of the type.
        // Otherwise callee-pass throughs of arguments can create return type
        // mismatches that block inlining.
        //
        // Note argument type mismatches that prevent inlining should
        // have been caught in impInlineInitVars.
        if (op1->TypeGet() != lclTyp)
        {
            op1->gtType = genActualType(lclTyp);
        }
    }
    else if (argInfo.argIsLclVar && !argCanBeModified && !argInfo.argHasCallerLocalRef && !argLosesTypeInfo)
    {
        // Directly substitute unaliased caller locals for args that cannot be modified
        //
        // Use the caller-supplied node if this is the first use.
        op1                = argNode;
        unsigned argLclNum = op1->AsLclVarCommon()->GetLclNum();
        argInfo.argTmpNum  = argLclNum;

        // Use an equivalent copy if this is the second or subsequent
        // use.
        //
        // Note argument type mismatches that prevent inlining should
        // have been caught in impInlineInitVars. If inlining is not prevented
        // but a cast is necessary, we similarly expect it to have been inserted then.
        // So here we may have argument type mismatches that are benign, for instance
        // passing a TYP_SHORT local (eg. normalized-on-load) as a TYP_INT arg.
        // The exception is when the inlining means we should start tracking the argument.
        if (argInfo.argIsUsed || ((lclTyp == TYP_BYREF) && !op1->TypeIs(TYP_BYREF)))
        {
            assert(op1->OperIs(GT_LCL_VAR));

            // Create a new lcl var node - remember the argument lclNum
            op1 = impCreateLocalNode(argLclNum DEBUGARG(op1->AsLclVar()->gtLclILoffs));
            // Start tracking things as a byref if the parameter is a byref.
            if (lclTyp == TYP_BYREF)
            {
                op1->gtType = TYP_BYREF;
            }
        }
    }
    else if (argInfo.argIsByRefToStructLocal && !argInfo.argHasStargOp)
    {
        /* Argument is a by-ref address to a struct, a normed struct, or its field.
           In these cases, don't spill the byref to a local, simply clone the tree and use it.
           This way we will increase the chance for this byref to be optimized away by
           a subsequent "dereference" operation.

           From Dev11 bug #139955: Argument node can also be TYP_I_IMPL if we've bashed the tree
           (in impInlineInitVars()), if the arg has argHasLdargaOp as well as argIsByRefToStructLocal.
           For example, if the caller is:
                ldloca.s   V_1  // V_1 is a local struct
                call       void Test.ILPart::RunLdargaOnPointerArg(int32*)
           and the callee being inlined has:
                .method public static void  RunLdargaOnPointerArg(int32* ptrToInts) cil managed
                    ldarga.s   ptrToInts
                    call       void Test.FourInts::NotInlined_SetExpectedValuesThroughPointerToPointer(int32**)
           then we change the argument tree (of "ldloca.s V_1") to TYP_I_IMPL to match the callee signature. We'll
           soon afterwards reject the inlining anyway, since the tree we return isn't a GT_LCL_VAR.
        */
        assert(argNode->TypeIs(TYP_BYREF, TYP_I_IMPL));
        op1 = gtCloneExpr(argNode);
    }
    else
    {
        /* Argument is a complex expression - it must be evaluated into a temp */

        if (argInfo.argHasTmp)
        {
            assert(argInfo.argIsUsed);
            assert(argInfo.argTmpNum < lvaCount);

            /* Create a new lcl var node - remember the argument lclNum */
            op1 = gtNewLclvNode(argInfo.argTmpNum, genActualType(lclTyp));

            /* This is the second or later use of the this argument,
            so we have to use the temp (instead of the actual arg) */
            argInfo.argBashTmpNode = nullptr;
        }
        else
        {
            /* First time use */
            assert(!argInfo.argIsUsed);

            /* Reserve a temp for the expression.
             * Use a large size node as we may change it later */

            const unsigned tmpNum = lvaGrabTemp(true DEBUGARG("Inlining Arg"));

            lvaTable[tmpNum].lvType = lclTyp;

            // If arg can't be modified, mark it as single def.
            // For ref types, determine the class of the arg temp.
            if (!argCanBeModified)
            {
                assert(lvaTable[tmpNum].lvSingleDef == 0);
                lvaTable[tmpNum].lvSingleDef = 1;
                JITDUMP("Marked V%02u as a single def temp\n", tmpNum);

                if (lclTyp == TYP_REF)
                {
                    // Use argNode type (when it exists) or lclInfo type
                    lvaSetClass(tmpNum, argNode, lclInfo.lclTypeHandle);
                }
            }
            else
            {
                if (lclTyp == TYP_REF)
                {
                    // Arg might be modified. Use the declared type of the argument.
                    lvaSetClass(tmpNum, lclInfo.lclTypeHandle);
                }
            }

            assert(!lvaTable[tmpNum].IsAddressExposed());
            if (argInfo.argHasLdargaOp)
            {
                lvaTable[tmpNum].lvHasLdAddrOp = 1;
            }

            if (varTypeIsStruct(lclTyp))
            {
                lvaSetStruct(tmpNum, lclInfo.lclTypeHandle, true /* unsafe value cls check */);
            }

            argInfo.argHasTmp = true;
            argInfo.argTmpNum = tmpNum;

            // If we require strict exception order, then arguments must
            // be evaluated in sequence before the body of the inlined method.
            // So we need to evaluate them to a temp.
            // Also, if arguments have global or local references, we need to
            // evaluate them to a temp before the inlined body as the
            // inlined body may be modifying the global ref.
            // TODO-1stClassStructs: We currently do not reuse an existing lclVar
            // if it is a struct, because it requires some additional handling.

            if ((!varTypeIsStruct(lclTyp) && !argInfo.argHasSideEff && !argInfo.argHasGlobRef &&
                 !argInfo.argHasCallerLocalRef))
            {
                /* Get a *LARGE* LCL_VAR node */
                op1 = gtNewLclLNode(tmpNum, genActualType(lclTyp));

                /* Record op1 as the very first use of this argument.
                If there are no further uses of the arg, we may be
                able to use the actual arg node instead of the temp.
                If we do see any further uses, we will clear this. */
                argInfo.argBashTmpNode = op1;
            }
            else
            {
                /* Get a small LCL_VAR node */
                op1 = gtNewLclvNode(tmpNum, genActualType(lclTyp));
                /* No bashing of this argument */
                argInfo.argBashTmpNode = nullptr;
            }
        }
    }

    // Mark this argument as used.
    argInfo.argIsUsed = true;

    return op1;
}

/******************************************************************************
 Is this the original "this" argument to the call being inlined?

 Note that we do not inline methods with "starg 0", and so we do not need to
 worry about it.
*/

bool Compiler::impInlineIsThis(GenTree* tree, InlArgInfo* inlArgInfo)
{
    assert(compIsForInlining());
    return (tree->OperIs(GT_LCL_VAR) && tree->AsLclVarCommon()->GetLclNum() == inlArgInfo[0].argTmpNum);
}

//-----------------------------------------------------------------------------
// impInlineIsGuaranteedThisDerefBeforeAnySideEffects: Check if a dereference in
// the inlinee can guarantee that the "this" pointer is non-NULL.
//
// Arguments:
//    additionalTree - a tree to check for side effects
//    additionalCallArgs - a list of call args to check for side effects
//    dereferencedAddress - address expression being dereferenced
//    inlArgInfo - inlinee argument information
//
// Notes:
//    If we haven't hit a branch or a side effect, and we are dereferencing
//    from 'this' to access a field or make GTF_CALL_NULLCHECK call,
//    then we can avoid a separate null pointer check.
//
//    The importer stack and current statement list are searched for side effects.
//    Trees that have been popped of the stack but haven't been appended to the
//    statement list and have to be checked for side effects may be provided via
//    additionalTree and additionalCallArgs.
//
bool Compiler::impInlineIsGuaranteedThisDerefBeforeAnySideEffects(GenTree*    additionalTree,
                                                                  CallArgs*   additionalCallArgs,
                                                                  GenTree*    dereferencedAddress,
                                                                  InlArgInfo* inlArgInfo)
{
    assert(compIsForInlining());
    assert(opts.OptEnabled(CLFLG_INLINING));

    BasicBlock* block = compCurBB;

    if (block != fgFirstBB)
    {
        return false;
    }

    if (!impInlineIsThis(dereferencedAddress, inlArgInfo))
    {
        return false;
    }

    if ((additionalTree != nullptr) && GTF_GLOBALLY_VISIBLE_SIDE_EFFECTS(additionalTree->gtFlags))
    {
        return false;
    }

    if (additionalCallArgs != nullptr)
    {
        for (CallArg& arg : additionalCallArgs->Args())
        {
            if (GTF_GLOBALLY_VISIBLE_SIDE_EFFECTS(arg.GetEarlyNode()->gtFlags))
            {
                return false;
            }
        }
    }

    for (Statement* stmt : StatementList(impStmtList))
    {
        GenTree* expr = stmt->GetRootNode();
        if (GTF_GLOBALLY_VISIBLE_SIDE_EFFECTS(expr->gtFlags))
        {
            return false;
        }
    }

    for (unsigned level = 0; level < stackState.esStackDepth; level++)
    {
        GenTreeFlags stackTreeFlags = stackState.esStack[level].val->gtFlags;
        if (GTF_GLOBALLY_VISIBLE_SIDE_EFFECTS(stackTreeFlags))
        {
            return false;
        }
    }

    return true;
}

//------------------------------------------------------------------------
// impAllocateMethodPointerInfo: create methodPointerInfo into jit-allocated memory and init it.
//
// Arguments:
//    token - init value for the allocated token.
//    tokenConstrained - init value for the constraint associated with the token
//
// Return Value:
//    pointer to token into jit-allocated memory.
methodPointerInfo* Compiler::impAllocateMethodPointerInfo(const CORINFO_RESOLVED_TOKEN& token, mdToken tokenConstrained)
{
    methodPointerInfo* memory = getAllocator(CMK_Unknown).allocate<methodPointerInfo>(1);
    memory->m_token           = token;
    memory->m_tokenConstraint = tokenConstrained;
    return memory;
}
