// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

using ILCompiler.DependencyAnalysis;

using Internal.IL;
using Internal.TypeSystem;
using Internal.TypeSystem.Ecma;

using CombinedDependencyList = System.Collections.Generic.List<ILCompiler.DependencyAnalysisFramework.DependencyNodeCore<ILCompiler.DependencyAnalysis.NodeFactory>.CombinedDependencyListEntry>;
using FlowAnnotations = ILLink.Shared.TrimAnalysis.FlowAnnotations;

namespace ILCompiler
{
    // Class that computes the initial state of static fields on a type by interpreting the static constructor.
    //
    // Values are represented by instances of an abstract Value class. Several specialized descendants of
    // the Value class exist, representing value types (including e.g. a specialized class representing
    // RuntimeFieldHandle), or reference types (including e.g. specialized class representing an array).
    //
    // For simplicity, non-reference values are represented as byte arrays. This requires many short lived array
    // allocations, but makes a lot of things simpler (e.g. byrefs to values are essentially free because they
    // only carry a reference to the original array and an optional index).
    //
    // When dealing with non-reference types (valuetypes and unmanaged pointers) we need to be careful
    // about assignment semantics. Some operations need to make a copy of the valuetype bytes while others
    // are fine to reuse the original byte array. Whenever storing a value into a location, we need to assign
    // a new value to the existing Value instance to keep byrefs working.
    public class TypePreinit
    {
        private readonly MetadataType _type;
        private readonly CompilationModuleGroup _compilationGroup;
        private readonly ILProvider _ilProvider;
        private readonly TypePreinitializationPolicy _policy;
        private readonly ReadOnlyFieldPolicy _readOnlyPolicy;
        private readonly FlowAnnotations _flowAnnotations;
        private readonly Dictionary<FieldDesc, Value> _fieldValues = new Dictionary<FieldDesc, Value>();
        private readonly Dictionary<string, StringInstance> _internedStrings = new Dictionary<string, StringInstance>();
        private readonly Dictionary<TypeDesc, RuntimeTypeValue> _internedTypes = new Dictionary<TypeDesc, RuntimeTypeValue>();
        private readonly Dictionary<MetadataType, NestedPreinitResult> _nestedPreinitResults = new Dictionary<MetadataType, NestedPreinitResult>();
        private readonly Dictionary<EcmaField, byte[]> _rvaFieldDatas = new Dictionary<EcmaField, byte[]>();

        private TypePreinit(MetadataType owningType, CompilationModuleGroup compilationGroup, ILProvider ilProvider, TypePreinitializationPolicy policy, ReadOnlyFieldPolicy readOnlyPolicy, FlowAnnotations flowAnnotations)
        {
            _type = owningType;
            _compilationGroup = compilationGroup;
            _ilProvider = ilProvider;
            _policy = policy;
            _readOnlyPolicy = readOnlyPolicy;
            _flowAnnotations = flowAnnotations;

            // Zero initialize all fields we model.
            foreach (var field in owningType.GetFields())
            {
                if (!field.IsStatic || field.IsLiteral || field.IsThreadStatic || field.HasRva)
                    continue;

                _fieldValues.Add(field, NewUninitializedLocationValue(field.FieldType, field));
            }
        }

        public static PreinitializationInfo ScanType(CompilationModuleGroup compilationGroup, ILProvider ilProvider, TypePreinitializationPolicy policy, ReadOnlyFieldPolicy readOnlyPolicy, FlowAnnotations flowAnnotations, MetadataType type)
        {
            Debug.Assert(type.HasStaticConstructor);
            Debug.Assert(!type.IsGenericDefinition);
            Debug.Assert(!type.IsRuntimeDeterminedSubtype);

            if (type.IsCanonicalSubtype(CanonicalFormKind.Any))
            {
                // It's an odd question to ask about canonical types. Defer to policy that might
                // have more information.
                // If the policy allows it, we allow it, but create invalid field values so that
                // things still crash if someone wanted to do more with canonical types than just
                // ask if a cctor check is necessary to access.
                if (policy.CanPreinitializeAllConcreteFormsForCanonForm(type))
                    return new PreinitializationInfo(type, Array.Empty<KeyValuePair<FieldDesc, ISerializableValue>>());

                return new PreinitializationInfo(type, "Disallowed by policy");
            }

            if (!policy.CanPreinitialize(type))
                return new PreinitializationInfo(type, "Disallowed by policy");

            TypePreinit preinit = null;

            Status status;
            try
            {
                preinit = new TypePreinit(type, compilationGroup, ilProvider, policy, readOnlyPolicy, flowAnnotations);
                int instructions = 0;
                status = preinit.TryScanMethod(type.GetStaticConstructor(), null, null, ref instructions, out _);
            }
            catch (TypeSystemException ex)
            {
                status = Status.Fail(type.GetStaticConstructor(), ex.Message);
            }

            if (status.IsSuccessful)
            {
                var values = new List<KeyValuePair<FieldDesc, ISerializableValue>>();
                foreach (var kvp in preinit._fieldValues)
                    values.Add(new KeyValuePair<FieldDesc, ISerializableValue>(kvp.Key, kvp.Value));

                return new PreinitializationInfo(type, values);
            }

            return new PreinitializationInfo(type, status.FailureReason);
        }

        private bool TryGetNestedPreinitResult(MethodDesc callingMethod, MetadataType type, Stack<MethodDesc> recursionProtect, ref int instructionCounter, out NestedPreinitResult result)
        {
            if (!_nestedPreinitResults.TryGetValue(type, out result))
            {
                TypePreinit nestedPreinit = new TypePreinit(type, _compilationGroup, _ilProvider, _policy, _readOnlyPolicy, _flowAnnotations);
                recursionProtect ??= new Stack<MethodDesc>();
                recursionProtect.Push(callingMethod);

                // Since we don't reset the instruction counter as we interpret the nested cctor,
                // remember the instruction counter before we start interpreting so that we can subtract
                // the instructions later when we convert object instances allocated in the nested
                // cctor to foreign instances in the currently analyzed cctor.
                // E.g. if the nested cctor allocates a new object at the beginning of the cctor,
                // we should treat it as a ForeignTypeInstance with allocation site ID 0, not allocation
                // site ID of `instructionCounter + 0`.
                // We could also reset the counter, but we use the instruction counter as a complexity cutoff
                // and resetting it would lead to unpredictable analysis durations.
                int baseInstructionCounter = instructionCounter;
                Status status = nestedPreinit.TryScanMethod(type.GetStaticConstructor(), null, recursionProtect, ref instructionCounter, out Value _);
                if (!status.IsSuccessful)
                {
                    result = default;
                    return false;
                }
                recursionProtect.Pop();

                result = new NestedPreinitResult(nestedPreinit._fieldValues, baseInstructionCounter);

                _nestedPreinitResults.Add(type, result);
            }

            return true;
        }

        private byte[] GetFieldRvaData(EcmaField field)
        {
            if (!_rvaFieldDatas.TryGetValue(field, out byte[] result))
                _rvaFieldDatas.Add(field, result = field.GetFieldRvaData());
            return result;
        }

        private Status TryScanMethod(MethodDesc method, Value[] parameters, Stack<MethodDesc> recursionProtect, ref int instructionCounter, out Value returnValue)
        {
            MethodIL methodIL = _ilProvider.GetMethodIL(method);
            if (methodIL == null)
            {
                returnValue = null;
                return Status.Fail(method, "Extern method");
            }

            return TryScanMethod(methodIL, parameters, recursionProtect, ref instructionCounter, out returnValue);
        }

        private Status TryScanMethod(MethodIL methodIL, Value[] parameters, Stack<MethodDesc> recursionProtect, ref int instructionCounter, out Value returnValue)
        {
            returnValue = default;

            if (recursionProtect != null && recursionProtect.Contains(methodIL.OwningMethod))
                return Status.Fail(methodIL.OwningMethod, "Recursion");

            ILExceptionRegion[] ehRegions = methodIL.GetExceptionRegions();
            if (ehRegions != null && ehRegions.Length > 0)
            {
                // We don't care about catch/filter/fault because those only run when an exception happens
                // (exceptions will never happen here). But finally needs to run in non-exceptional paths
                // and we don't model that yet.
                foreach (ILExceptionRegion ehRegion in ehRegions)
                {
                    if (ehRegion.Kind == ILExceptionRegionKind.Finally)
                        return Status.Fail(methodIL.OwningMethod, "Finally regions");
                }
            }

            var reader = new ILReader(methodIL.GetILBytes());

            TypeSystemContext context = methodIL.OwningMethod.Context;

            var stack = new Stack(methodIL.MaxStack, context.Target);

            LocalVariableDefinition[] localTypes = methodIL.GetLocals();
            Value[] locals = new Value[localTypes.Length];
            for (int i = 0; i < localTypes.Length; i++)
            {
                locals[i] = NewUninitializedLocationValue(localTypes[i].Type, fieldThatOwnsMemory: null);
            }

            // Read IL opcodes and interpret their semantics.
            //
            // This is not a full interpreter and we're allowed to not interpret everything. If a semantic is
            // not implemented by the interpreter, we simply fail.
            //
            // We also need to do basic sanity checking for invalid IL to protect us from crashing. These
            // all throw the TypeSystem's InvalidProgramException. The exception doesn't need to exactly match
            // the runtime exception. We just need something reasonably catchable to abort interpreting.
            //
            // We throw instead of returning false to aid debuggability of the interpreter (we shouldn't see
            // exceptions in normal code so an exception is usually a bug).

            while (reader.HasNext)
            {
                if (instructionCounter == 100000)
                    return Status.Fail(methodIL.OwningMethod, "Instruction limit");

                instructionCounter++;

                TypeDesc constrainedType = null;

            again:
                ILOpcode opcode = reader.ReadILOpcode();
                switch (opcode)
                {
                    case ILOpcode.ldc_i4_m1:
                    case ILOpcode.ldc_i4_s:
                    case ILOpcode.ldc_i4:
                    case ILOpcode.ldc_i4_0:
                    case ILOpcode.ldc_i4_1:
                    case ILOpcode.ldc_i4_2:
                    case ILOpcode.ldc_i4_3:
                    case ILOpcode.ldc_i4_4:
                    case ILOpcode.ldc_i4_5:
                    case ILOpcode.ldc_i4_6:
                    case ILOpcode.ldc_i4_7:
                    case ILOpcode.ldc_i4_8:
                    {
                        int value = opcode switch
                        {
                            ILOpcode.ldc_i4_m1 => -1,
                            ILOpcode.ldc_i4_s => (sbyte)reader.ReadILByte(),
                            ILOpcode.ldc_i4 => (int)reader.ReadILUInt32(),
                            _ => opcode - ILOpcode.ldc_i4_0,
                        };
                        stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32(value));
                    }
                    break;

                    case ILOpcode.ldc_i8:
                        stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64((long)reader.ReadILUInt64()));
                        break;

                    case ILOpcode.ldc_r4:
                    case ILOpcode.ldc_r8:
                        stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble(
                            opcode == ILOpcode.ldc_r4 ? reader.ReadILFloat() : reader.ReadILDouble()));
                        break;

                    case ILOpcode.sizeof_:
                    {
                        TypeDesc type = (TypeDesc)methodIL.GetObject(reader.ReadILToken());
                        stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32(type.GetElementSize().AsInt));
                    }
                    break;

                    case ILOpcode.ldnull:
                        stack.Push((ReferenceTypeValue)null);
                        break;

                    case ILOpcode.newarr:
                    {
                        if (!stack.TryPopIntValue(out int elementCount))
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        const int MaximumInterpretedArraySize = 8192;

                        TypeDesc elementType = (TypeDesc)methodIL.GetObject(reader.ReadILToken());
                        if (elementCount > 0
                            && (elementType.IsGCPointer
                            || (elementType.IsValueType && ((DefType)elementType).ContainsGCPointers)))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "GC pointers");
                        }

                        if (elementCount < 0
                            || elementCount > MaximumInterpretedArraySize)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Array out of bounds");
                        }

                        if (elementType.RequiresAlign8())
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Align8");
                        }

                        AllocationSite allocSite = new AllocationSite(_type, instructionCounter);
                        stack.Push(new ArrayInstance(elementType.MakeArrayType(), elementCount, allocSite));
                    }
                    break;

                    case ILOpcode.dup:
                        if (stack.Count == 0)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        stack.Push(stack.Peek());
                        break;

                    case ILOpcode.pop:
                    {
                        stack.Pop();
                        break;
                    }

                    case ILOpcode.ldstr:
                    {
                        string s = (string)methodIL.GetObject(reader.ReadILToken());
                        if (!_internedStrings.TryGetValue(s, out StringInstance instance))
                        {
                            instance = new StringInstance(context.GetWellKnownType(WellKnownType.String), s);
                            _internedStrings.Add(s, instance);
                        }
                        stack.Push(instance);
                    }
                    break;

                    case ILOpcode.ret:
                    {
                        bool returnsVoid = methodIL.OwningMethod.Signature.ReturnType.IsVoid;
                        if ((returnsVoid && stack.Count > 0)
                            || (!returnsVoid && stack.Count != 1))
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        if (!returnsVoid)
                        {
                            returnValue = stack.PopIntoLocation(methodIL.OwningMethod.Signature.ReturnType);
                        }
                        return Status.Success;
                    }

                    case ILOpcode.nop:
                    case ILOpcode.volatile_:
                        break;

                    case ILOpcode.stsfld:
                    {
                        FieldDesc field = (FieldDesc)methodIL.GetObject(reader.ReadILToken());
                        if (!field.IsStatic || field.IsLiteral)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        if (field.OwningType != _type)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Store into other static");
                        }

                        if (field.IsThreadStatic || field.HasRva)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported static");
                        }

                        if (_flowAnnotations.RequiresDataflowAnalysisDueToSignature(field))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Needs dataflow analysis");
                        }

                        if (_fieldValues[field] is IAssignableValue assignableField)
                        {
                            if (!assignableField.TryAssign(stack.PopIntoLocation(field.FieldType)))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported store");
                            }
                        }
                        else
                        {
                            Value value = stack.PopIntoLocation(field.FieldType);
                            if (value is IInternalModelingOnlyValue)
                                return Status.Fail(methodIL.OwningMethod, opcode, "Value with no external representation");
                            _fieldValues[field] = value;
                        }
                    }
                    break;

                    case ILOpcode.ldsfld:
                    case ILOpcode.ldsflda:
                    {
                        FieldDesc field = (FieldDesc)methodIL.GetObject(reader.ReadILToken());
                        if (!field.IsStatic || field.IsLiteral)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        if (field.IsThreadStatic)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported static");
                        }

                        if (opcode != ILOpcode.ldsfld
                            && _flowAnnotations.RequiresDataflowAnalysisDueToSignature(field))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Needs dataflow analysis");
                        }

                        Value fieldValue;
                        if (field.HasRva)
                        {
                            if (!field.IsInitOnly
                                || field.OwningType.HasStaticConstructor
                                || field.GetTypicalFieldDefinition() is not EcmaField ecmaField)
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported RVA static");
                            }

                            fieldValue = new ValueTypeValue(GetFieldRvaData(ecmaField));
                        }
                        else if (field.OwningType == _type)
                        {
                            fieldValue = _fieldValues[field];
                        }
                        else if (_readOnlyPolicy.IsReadOnly(field)
                            && field.OwningType.HasStaticConstructor
                            && _policy.CanPreinitialize(field.OwningType))
                        {
                            if (!TryGetNestedPreinitResult(methodIL.OwningMethod, (MetadataType)field.OwningType, recursionProtect, ref instructionCounter, out NestedPreinitResult nestedPreinitResult))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Nested cctor failed to preinit");
                            }

                            if (!nestedPreinitResult.TryGetFieldValue(this, field, out fieldValue))
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else if (_readOnlyPolicy.IsReadOnly(field)
                            && opcode != ILOpcode.ldsflda // We need to intern these for correctness in ldsfda scenarios
                            && !field.OwningType.HasStaticConstructor)
                        {
                            // (Effectively) read only field but no static constructor to set it: the value is default-initialized.
                            fieldValue = NewUninitializedLocationValue(field.FieldType, field);
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Load from other non-initonly static");
                        }

                        if (opcode == ILOpcode.ldsfld)
                        {
                            stack.PushFromLocation(field.FieldType, fieldValue);
                        }
                        else
                        {
                            Debug.Assert(opcode == ILOpcode.ldsflda);
                            if (fieldValue == null || !fieldValue.TryCreateByRef(out Value byRefValue))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported byref");
                            }

                            stack.Push(StackValueKind.ByRef, byRefValue);
                        }
                    }
                    break;

                    case ILOpcode.call:
                    case ILOpcode.callvirt:
                    {
                        MethodDesc method = (MethodDesc)methodIL.GetObject(reader.ReadILToken());
                        MethodSignature methodSig = method.Signature;
                        int paramOffset = methodSig.IsStatic ? 0 : 1;
                        int numParams = methodSig.Length + paramOffset;

                        if (constrainedType != null)
                        {
                            DefaultInterfaceMethodResolution staticResolution = default;
                            MethodDesc directMethod = constrainedType.GetClosestDefType().TryResolveConstraintMethodApprox(method.OwningType, method, out bool forceUseRuntimeLookup, ref staticResolution);
                            if (directMethod == null || forceUseRuntimeLookup)
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Did not resolve constraint");
                            }
                            method = directMethod;
                        }

                        TypeDesc owningType = method.OwningType;
                        if (!_compilationGroup.CanInline(methodIL.OwningMethod, method))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Cannot inline");
                        }

                        if (owningType.HasStaticConstructor
                                && owningType != methodIL.OwningMethod.OwningType
                                && (method.Signature.IsStatic || method.IsConstructor || owningType.IsValueType || owningType.IsInterface) // ECMA-335 I.8.9.5
                                && !((MetadataType)owningType).IsBeforeFieldInit)
                        {
                            // Static constructor needs to execute before we do the call. If we can preinitialize, consider it executed,
                            // otherwise there might be side effects we'd miss by letting this through.
                            if (!TryGetNestedPreinitResult(methodIL.OwningMethod, (MetadataType)owningType, recursionProtect, ref instructionCounter, out _))
                                return Status.Fail(methodIL.OwningMethod, opcode, "Static constructor");
                        }

                        if (_flowAnnotations.RequiresDataflowAnalysisDueToSignature(method))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Needs dataflow analysis");
                        }

                        Value[] methodParams = new Value[numParams];
                        for (int i = numParams - 1; i >= 0; i--)
                        {
                            methodParams[i] = stack.PopIntoLocation(GetArgType(method, i));
                        }

                        if (opcode == ILOpcode.callvirt)
                        {
                            // Only support non-virtual methods for now + we don't emulate NRE on null this
                            if (!owningType.IsValueType && (method.IsVirtual || methodParams[0] == null))
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        Value retVal;
                        if (!method.IsIntrinsic || !TryHandleIntrinsicCall(method, methodParams, out retVal))
                        {
                            recursionProtect ??= new Stack<MethodDesc>();
                            recursionProtect.Push(methodIL.OwningMethod);
                            Status callResult = TryScanMethod(method, methodParams, recursionProtect, ref instructionCounter, out retVal);
                            if (!callResult.IsSuccessful)
                            {
                                recursionProtect.Pop();
                                return callResult;
                            }
                            recursionProtect.Pop();
                        }

                        if (!methodSig.ReturnType.IsVoid)
                            stack.PushFromLocation(methodSig.ReturnType, retVal);
                    }
                    break;

                    case ILOpcode.newobj:
                    {
                        MethodDesc ctor = (MethodDesc)methodIL.GetObject(reader.ReadILToken());
                        MethodSignature ctorSig = ctor.Signature;

                        TypeDesc owningType = ctor.OwningType;
                        if (!_compilationGroup.CanInline(methodIL.OwningMethod, ctor)
                            || !_compilationGroup.ContainsType(owningType))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Cannot inline");
                        }

                        if (owningType.HasStaticConstructor
                                && owningType != methodIL.OwningMethod.OwningType
                                && !((MetadataType)owningType).IsBeforeFieldInit)
                        {
                            // Static constructor needs to execute before we do the call. If we can preinitialize, consider it executed,
                            // otherwise there might be side effects we'd miss by letting this through.
                            if (!TryGetNestedPreinitResult(methodIL.OwningMethod, (MetadataType)owningType, recursionProtect, ref instructionCounter, out _))
                                return Status.Fail(methodIL.OwningMethod, opcode, "Static constructor");
                        }

                        if (!owningType.IsDefType)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Not a class or struct");
                        }

                        if (owningType.HasFinalizer)
                        {
                            // We have a finalizer. There's still a small chance it has been nopped out
                            // with a feature switch. Check for that.
                            byte[] finalizerMethodILBytes = _ilProvider.GetMethodIL(owningType.GetFinalizer()).GetILBytes();
                            if (finalizerMethodILBytes.Length != 1 || finalizerMethodILBytes[0] != (byte)ILOpcode.ret)
                            {
                                // Finalizer might have observable side effects
                                return Status.Fail(methodIL.OwningMethod, opcode, "Finalizable class");
                            }
                        }

                        if (_flowAnnotations.RequiresDataflowAnalysisDueToSignature(ctor))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Needs dataflow analysis");
                        }

                        if (owningType.RequiresAlign8())
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Align8");
                        }

                        Value[] ctorParameters = new Value[ctorSig.Length + 1];
                        for (int i = ctorSig.Length - 1; i >= 0; i--)
                        {
                            ctorParameters[i + 1] = stack.PopIntoLocation(GetArgType(ctor, i + 1));
                        }

                        AllocationSite allocSite = new AllocationSite(_type, instructionCounter);

                        Value instance;
                        if (owningType.IsDelegate)
                        {
                            if (!(ctorParameters[2] is MethodPointerValue methodPointer))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Unverifiable delegate creation");
                            }

                            ReferenceTypeValue firstParameter = null;
                            if (ctorParameters[1] != null)
                            {
                                firstParameter = ctorParameters[1] as ReferenceTypeValue;
                                if (firstParameter == null)
                                {
                                    ThrowHelper.ThrowInvalidProgramException();
                                }
                            }

                            MethodDesc pointedMethod = methodPointer.PointedToMethod;
                            if ((firstParameter == null) != pointedMethod.Signature.IsStatic)
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Open/closed static/instance delegate mismatch");
                            }

                            if (firstParameter != null && pointedMethod.HasInstantiation)
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Delegate with fat pointer");
                            }

                            instance = new DelegateInstance(owningType, pointedMethod, firstParameter, allocSite);
                        }
                        else
                        {
                            if (owningType.IsValueType)
                            {
                                instance = NewUninitializedLocationValue(owningType, fieldThatOwnsMemory: null);
                                if (!instance.TryCreateByRef(out ctorParameters[0]))
                                {
                                    return Status.Fail(methodIL.OwningMethod, opcode, "Can't make `this`");
                                }
                            }
                            else
                            {
                                instance = new ObjectInstance((DefType)owningType, allocSite);
                                ctorParameters[0] = instance;
                            }

                            if (((DefType)owningType).ContainsGCPointers)
                            {
                                // We don't want to end up with GC pointers in the frozen region
                                // because write barriers can't handle that.

                                // We can make an exception for readonly fields.
                                bool allGcPointersAreReadonly = true;
                                TypeDesc currentType = owningType;
                                do
                                {
                                    foreach (FieldDesc field in currentType.GetFields())
                                    {
                                        if (field.IsStatic)
                                            continue;

                                        TypeDesc fieldType = field.FieldType;
                                        if (fieldType.IsGCPointer)
                                        {
                                            if (!_readOnlyPolicy.IsReadOnly(field))
                                            {
                                                allGcPointersAreReadonly = false;
                                                break;
                                            }
                                        }
                                        else if (fieldType.IsValueType && ((DefType)fieldType).ContainsGCPointers)
                                        {
                                            allGcPointersAreReadonly = false;
                                            break;
                                        }
                                    }
                                } while (allGcPointersAreReadonly && (currentType = currentType.BaseType) != null && !currentType.IsValueType);

                                if (!allGcPointersAreReadonly)
                                    return Status.Fail(methodIL.OwningMethod, opcode, "GC pointers");
                            }

                            recursionProtect ??= new Stack<MethodDesc>();
                            recursionProtect.Push(methodIL.OwningMethod);
                            Status ctorCallResult = TryScanMethod(ctor, ctorParameters, recursionProtect, ref instructionCounter, out _);
                            if (!ctorCallResult.IsSuccessful)
                            {
                                recursionProtect.Pop();
                                return ctorCallResult;
                            }

                            recursionProtect.Pop();
                        }

                        stack.PushFromLocation(owningType, instance);
                    }
                    break;

                    case ILOpcode.localloc:
                    {
                        StackEntry entry = stack.Pop();
                        long size = entry.ValueKind switch
                        {
                            StackValueKind.Int32 => entry.Value.AsInt32(),
                            StackValueKind.NativeInt => (context.Target.PointerSize == 4)
                                ? entry.Value.AsInt32() : entry.Value.AsInt64(),
                            _ => long.MaxValue
                        };

                        // Arbitrary limit for allocation size to prevent compiler OOM
                        if (size < 0 || size > 8192)
                            return Status.Fail(methodIL.OwningMethod, ILOpcode.localloc);

                        stack.Push(StackValueKind.NativeInt, new ByRefValue(new byte[size], pointedToOffset: 0));
                    }
                    break;

                    case ILOpcode.stfld:
                    {
                        FieldDesc field = (FieldDesc)methodIL.GetObject(reader.ReadILToken());

                        if (field.IsStatic)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Static field with stfld");
                        }

                        Value value = stack.PopIntoLocation(field.FieldType);
                        StackEntry instance = stack.Pop();

                        if (field.FieldType.IsGCPointer && value != null)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Reference field");
                        }

                        if (_flowAnnotations.RequiresDataflowAnalysisDueToSignature(field))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Needs dataflow analysis");
                        }

                        if (instance.Value is not IHasInstanceFields settableInstance
                            || !settableInstance.TrySetField(field, value))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Not settable");
                        }
                    }
                    break;

                    case ILOpcode.ldfld:
                    {
                        FieldDesc field = (FieldDesc)methodIL.GetObject(reader.ReadILToken());

                        if (field.FieldType.IsGCPointer
                            || field.IsStatic)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        StackEntry instance = stack.Pop();

                        var loadableInstance = instance.Value as IHasInstanceFields;
                        if (loadableInstance == null)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        Value fieldValue = loadableInstance.GetField(field);

                        stack.PushFromLocation(field.FieldType, fieldValue);
                    }
                    break;

                    case ILOpcode.ldflda:
                    {
                        FieldDesc field = (FieldDesc)methodIL.GetObject(reader.ReadILToken());
                        if (field.FieldType.IsGCPointer
                            || field.IsStatic)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        if (_flowAnnotations.RequiresDataflowAnalysisDueToSignature(field))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Needs dataflow analysis");
                        }

                        StackEntry instance = stack.Pop();

                        var loadableInstance = instance.Value as IHasInstanceFields;
                        if (loadableInstance == null)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        stack.Push(StackValueKind.ByRef, loadableInstance.GetFieldAddress(field));
                    }
                    break;

                    case ILOpcode.conv_i:
                    case ILOpcode.conv_u:
                    case ILOpcode.conv_i1:
                    case ILOpcode.conv_i2:
                    case ILOpcode.conv_i4:
                    case ILOpcode.conv_i8:
                    case ILOpcode.conv_u1:
                    case ILOpcode.conv_u2:
                    case ILOpcode.conv_u4:
                    case ILOpcode.conv_u8:
                    case ILOpcode.conv_r4:
                    case ILOpcode.conv_r8:
                    {
                        StackEntry popped = stack.Pop();

                        if (opcode is ILOpcode.conv_i or ILOpcode.conv_u
                            && popped.ValueKind == StackValueKind.ByRef)
                        {
                            Debug.Assert(popped.Value is ByRefValueBase);
                            stack.Push(StackValueKind.NativeInt, popped.Value);
                        }
                        else if (popped.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32)
                        {
                            int val = popped.Value.AsInt32();
                            switch (opcode)
                            {
                                case ILOpcode.conv_i:
                                    stack.Push(StackValueKind.NativeInt,
                                        context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64(val) : ValueTypeValue.FromInt32(val));
                                    break;
                                case ILOpcode.conv_u:
                                    stack.Push(StackValueKind.NativeInt,
                                        context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64((uint)val) : ValueTypeValue.FromInt32(val));
                                    break;
                                case ILOpcode.conv_i1:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((sbyte)val));
                                    break;
                                case ILOpcode.conv_i2:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((short)val));
                                    break;
                                case ILOpcode.conv_i4:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32(val));
                                    break;
                                case ILOpcode.conv_i8:
                                    stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64(val));
                                    break;
                                case ILOpcode.conv_u1:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((byte)val));
                                    break;
                                case ILOpcode.conv_u2:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((ushort)val));
                                    break;
                                case ILOpcode.conv_u4:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32(val));
                                    break;
                                case ILOpcode.conv_u8:
                                    stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64((uint)val));
                                    break;
                                case ILOpcode.conv_r4:
                                    stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble((float)val));
                                    break;
                                case ILOpcode.conv_r8:
                                    stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble((double)val));
                                    break;
                                default:
                                    return Status.Fail(methodIL.OwningMethod, opcode);
                            }
                        }
                        else if (popped.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64)
                        {
                            long val = popped.Value.AsInt64();
                            switch (opcode)
                            {
                                case ILOpcode.conv_u:
                                case ILOpcode.conv_i:
                                    stack.Push(StackValueKind.NativeInt,
                                        context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64(val) : ValueTypeValue.FromInt32((int)val));
                                    break;
                                case ILOpcode.conv_i1:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((sbyte)val));
                                    break;
                                case ILOpcode.conv_i2:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((short)val));
                                    break;
                                case ILOpcode.conv_i4:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((int)val));
                                    break;
                                case ILOpcode.conv_i8:
                                    stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64(val));
                                    break;
                                case ILOpcode.conv_u1:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((byte)val));
                                    break;
                                case ILOpcode.conv_u2:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((ushort)val));
                                    break;
                                case ILOpcode.conv_u4:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((int)val));
                                    break;
                                case ILOpcode.conv_u8:
                                    stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64(val));
                                    break;
                                case ILOpcode.conv_r4:
                                    stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble((float)val));
                                    break;
                                case ILOpcode.conv_r8:
                                    stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble((double)val));
                                    break;
                                default:
                                    return Status.Fail(methodIL.OwningMethod, opcode);
                            }
                        }
                        else if (popped.ValueKind == StackValueKind.Float)
                        {
                            double val = popped.Value.AsDouble();
                            switch (opcode)
                            {
                                case ILOpcode.conv_i:
                                    stack.Push(StackValueKind.NativeInt,
                                        context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64((long)val) : ValueTypeValue.FromInt32((int)val));
                                    break;
                                case ILOpcode.conv_u:
                                    stack.Push(StackValueKind.NativeInt,
                                        context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64((long)(ulong)val) : ValueTypeValue.FromInt32((int)(uint)val));
                                    break;
                                case ILOpcode.conv_i1:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((sbyte)val));
                                    break;
                                case ILOpcode.conv_i2:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((short)val));
                                    break;
                                case ILOpcode.conv_i4:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((int)val));
                                    break;
                                case ILOpcode.conv_i8:
                                    stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64((long)val));
                                    break;
                                case ILOpcode.conv_u1:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((byte)val));
                                    break;
                                case ILOpcode.conv_u2:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((ushort)val));
                                    break;
                                case ILOpcode.conv_u4:
                                    stack.Push(StackValueKind.Int32, ValueTypeValue.FromInt32((int)(uint)val));
                                    break;
                                case ILOpcode.conv_u8:
                                    stack.Push(StackValueKind.Int64, ValueTypeValue.FromInt64((long)(ulong)val));
                                    break;
                                case ILOpcode.conv_r4:
                                    stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble((float)val));
                                    break;
                                case ILOpcode.conv_r8:
                                    stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble(val));
                                    break;
                                default:
                                    return Status.Fail(methodIL.OwningMethod, opcode);
                            }
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                    }
                    break;

                    case ILOpcode.ldarg_0:
                    case ILOpcode.ldarg_1:
                    case ILOpcode.ldarg_2:
                    case ILOpcode.ldarg_3:
                    case ILOpcode.ldarg_s:
                    case ILOpcode.ldarg:
                    {
                        int index = opcode switch
                        {
                            ILOpcode.ldarg_s => reader.ReadILByte(),
                            ILOpcode.ldarg => reader.ReadILUInt16(),
                            _ => opcode - ILOpcode.ldarg_0,
                        };
                        stack.PushFromLocation(GetArgType(methodIL.OwningMethod, index), parameters[index]);
                    }
                    break;

                    case ILOpcode.starg_s:
                    case ILOpcode.starg:
                    {
                        int index = opcode == ILOpcode.starg ? reader.ReadILUInt16() : reader.ReadILByte();
                        TypeDesc argType = GetArgType(methodIL.OwningMethod, index);
                        if (parameters[index] is IAssignableValue assignableParam)
                        {
                            if (!assignableParam.TryAssign(stack.PopIntoLocation(argType)))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported store");
                            }
                        }
                        else
                            parameters[index] = stack.PopIntoLocation(argType);
                    }
                    break;

                    case ILOpcode.ldtoken:
                    {
                        var token = methodIL.GetObject(reader.ReadILToken());
                        if (token is FieldDesc field)
                        {
                            stack.Push(new StackEntry(StackValueKind.ValueType, new RuntimeFieldHandleValue(field)));
                        }
                        else if (token is TypeDesc type)
                        {
                            stack.Push(new StackEntry(StackValueKind.ValueType, new RuntimeTypeHandleValue(type)));
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                    }
                    break;

                    case ILOpcode.ldftn:
                    {
                        if (constrainedType != null)
                            return Status.Fail(methodIL.OwningMethod, ILOpcode.constrained);

                        var method = methodIL.GetObject(reader.ReadILToken()) as MethodDesc;
                        if (method != null)
                            stack.Push(StackValueKind.NativeInt, new MethodPointerValue(method));
                        else
                            ThrowHelper.ThrowInvalidProgramException();
                    }
                    break;

                    case ILOpcode.ldloc_0:
                    case ILOpcode.ldloc_1:
                    case ILOpcode.ldloc_2:
                    case ILOpcode.ldloc_3:
                    case ILOpcode.ldloc_s:
                    case ILOpcode.ldloc:
                    {
                        int index = opcode switch
                        {
                            ILOpcode.ldloc_s => reader.ReadILByte(),
                            ILOpcode.ldloc => reader.ReadILUInt16(),
                            _ => opcode - ILOpcode.ldloc_0,
                        };

                        if (index >= locals.Length)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        stack.PushFromLocation(localTypes[index].Type, locals[index]);
                    }
                    break;

                    case ILOpcode.stloc_0:
                    case ILOpcode.stloc_1:
                    case ILOpcode.stloc_2:
                    case ILOpcode.stloc_3:
                    case ILOpcode.stloc_s:
                    case ILOpcode.stloc:
                    {
                        int index = opcode switch
                        {
                            ILOpcode.stloc_s => reader.ReadILByte(),
                            ILOpcode.stloc => reader.ReadILUInt16(),
                            _ => opcode - ILOpcode.stloc_0,
                        };

                        if (index >= locals.Length)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        TypeDesc localType = localTypes[index].Type;
                        if (locals[index] is IAssignableValue assignableLocal)
                        {
                            if (!assignableLocal.TryAssign(stack.PopIntoLocation(localType)))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode, "Unsupported store");
                            }
                        }
                        else
                            locals[index] = stack.PopIntoLocation(localType);

                    }
                    break;

                    case ILOpcode.ldarga_s:
                    case ILOpcode.ldarga:
                    case ILOpcode.ldloca_s:
                    case ILOpcode.ldloca:
                    {
                        int index = opcode switch
                        {
                            ILOpcode.ldloca_s or ILOpcode.ldarga_s => reader.ReadILByte(),
                            ILOpcode.ldloca or ILOpcode.ldarga => reader.ReadILUInt16(),
                            _ => throw new NotImplementedException(), // Unreachable
                        };

                        Value[] storage = opcode is ILOpcode.ldloca or ILOpcode.ldloca_s ? locals : parameters;
                        if (index >= storage.Length)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        Value localValue = storage[index];
                        if (localValue == null || !localValue.TryCreateByRef(out Value byrefValue))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else
                        {
                            stack.Push(StackValueKind.ByRef, byrefValue);
                        }
                    }
                    break;

                    case ILOpcode.initobj:
                    {
                        StackEntry popped = stack.Pop();
                        if (popped.ValueKind != StackValueKind.ByRef)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        TypeDesc token = (TypeDesc)methodIL.GetObject(reader.ReadILToken());
                        if (token.IsGCPointer
                            || popped.Value is not ByRefValueBase byrefVal
                            || !byrefVal.TryInitialize(token.GetElementSize().AsInt))
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                    }
                    break;

                    case ILOpcode.br:
                    case ILOpcode.brfalse:
                    case ILOpcode.brtrue:
                    case ILOpcode.blt:
                    case ILOpcode.blt_un:
                    case ILOpcode.bgt:
                    case ILOpcode.bgt_un:
                    case ILOpcode.beq:
                    case ILOpcode.bne_un:
                    case ILOpcode.bge:
                    case ILOpcode.bge_un:
                    case ILOpcode.ble:
                    case ILOpcode.ble_un:
                    case ILOpcode.br_s:
                    case ILOpcode.brfalse_s:
                    case ILOpcode.brtrue_s:
                    case ILOpcode.blt_s:
                    case ILOpcode.blt_un_s:
                    case ILOpcode.bgt_s:
                    case ILOpcode.bgt_un_s:
                    case ILOpcode.beq_s:
                    case ILOpcode.bne_un_s:
                    case ILOpcode.bge_s:
                    case ILOpcode.bge_un_s:
                    case ILOpcode.ble_s:
                    case ILOpcode.ble_un_s:
                    {
                        int delta = opcode >= ILOpcode.br ?
                            (int)reader.ReadILUInt32() :
                            (sbyte)reader.ReadILByte();
                        int target = reader.Offset + delta;
                        if (target < 0
                            || target > reader.Size)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        ILOpcode normalizedOpcode = opcode >= ILOpcode.br ?
                            opcode - ILOpcode.br + ILOpcode.br_s :
                            opcode;

                        bool branchTaken;
                        if (normalizedOpcode == ILOpcode.brtrue_s || normalizedOpcode == ILOpcode.brfalse_s)
                        {
                            StackEntry condition = stack.Pop();
                            if (condition.ValueKind == StackValueKind.Int32 || (condition.ValueKind == StackValueKind.NativeInt && context.Target.PointerSize == 4))
                                branchTaken = normalizedOpcode == ILOpcode.brfalse_s
                                    ? condition.Value.AsInt32() == 0 : condition.Value.AsInt32() != 0;
                            else if (condition.ValueKind == StackValueKind.Int64 || (condition.ValueKind == StackValueKind.NativeInt && context.Target.PointerSize == 8))
                                branchTaken = normalizedOpcode == ILOpcode.brfalse_s
                                    ? condition.Value.AsInt64() == 0 : condition.Value.AsInt64() != 0;
                            else if (condition.ValueKind == StackValueKind.ObjRef)
                                branchTaken = normalizedOpcode == ILOpcode.brfalse_s
                                    ? condition.Value == null : condition.Value != null;
                            else
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else if (normalizedOpcode == ILOpcode.blt_s || normalizedOpcode == ILOpcode.bgt_s
                            || normalizedOpcode == ILOpcode.bge_s || normalizedOpcode == ILOpcode.beq_s
                            || normalizedOpcode == ILOpcode.ble_s || normalizedOpcode == ILOpcode.blt_un_s
                            || normalizedOpcode == ILOpcode.ble_un_s || normalizedOpcode == ILOpcode.bge_un_s
                            || normalizedOpcode == ILOpcode.bgt_un_s || normalizedOpcode == ILOpcode.bne_un_s)
                        {
                            StackEntry value2 = stack.Pop();
                            StackEntry value1 = stack.Pop();

                            if (value1.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32 && value2.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32)
                            {
                                branchTaken = normalizedOpcode switch
                                {
                                    ILOpcode.blt_s => value1.Value.AsInt32() < value2.Value.AsInt32(),
                                    ILOpcode.blt_un_s => (uint)value1.Value.AsInt32() < (uint)value2.Value.AsInt32(),
                                    ILOpcode.bgt_s => value1.Value.AsInt32() > value2.Value.AsInt32(),
                                    ILOpcode.bgt_un_s => (uint)value1.Value.AsInt32() > (uint)value2.Value.AsInt32(),
                                    ILOpcode.bge_s => value1.Value.AsInt32() >= value2.Value.AsInt32(),
                                    ILOpcode.bge_un_s => (uint)value1.Value.AsInt32() >= (uint)value2.Value.AsInt32(),
                                    ILOpcode.beq_s => value1.Value.AsInt32() == value2.Value.AsInt32(),
                                    ILOpcode.bne_un_s => value1.Value.AsInt32() != value2.Value.AsInt32(),
                                    ILOpcode.ble_s => value1.Value.AsInt32() <= value2.Value.AsInt32(),
                                    ILOpcode.ble_un_s => (uint)value1.Value.AsInt32() <= (uint)value2.Value.AsInt32(),
                                    _ => throw new NotImplementedException() // unreachable
                                };
                            }
                            else if (value1.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64 && value2.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64)
                            {
                                branchTaken = normalizedOpcode switch
                                {
                                    ILOpcode.blt_s => value1.Value.AsInt64() < value2.Value.AsInt64(),
                                    ILOpcode.blt_un_s => (ulong)value1.Value.AsInt64() < (ulong)value2.Value.AsInt64(),
                                    ILOpcode.bgt_s => value1.Value.AsInt64() > value2.Value.AsInt64(),
                                    ILOpcode.bgt_un_s => (ulong)value1.Value.AsInt64() > (ulong)value2.Value.AsInt64(),
                                    ILOpcode.bge_s => value1.Value.AsInt64() >= value2.Value.AsInt64(),
                                    ILOpcode.bge_un_s => (ulong)value1.Value.AsInt64() >= (ulong)value2.Value.AsInt64(),
                                    ILOpcode.beq_s => value1.Value.AsInt64() == value2.Value.AsInt64(),
                                    ILOpcode.bne_un_s => value1.Value.AsInt64() != value2.Value.AsInt64(),
                                    ILOpcode.ble_s => value1.Value.AsInt64() <= value2.Value.AsInt64(),
                                    ILOpcode.ble_un_s => (ulong)value1.Value.AsInt64() <= (ulong)value2.Value.AsInt64(),
                                    _ => throw new NotImplementedException() // unreachable
                                };
                            }
                            else if (value1.ValueKind == StackValueKind.Float && value2.ValueKind == StackValueKind.Float)
                            {
                                branchTaken = normalizedOpcode switch
                                {
                                    ILOpcode.blt_s => value1.Value.AsDouble() < value2.Value.AsDouble(),
                                    ILOpcode.blt_un_s => !(value1.Value.AsDouble() >= value2.Value.AsDouble()),
                                    ILOpcode.bgt_s => value1.Value.AsDouble() > value2.Value.AsDouble(),
                                    ILOpcode.bgt_un_s => !(value1.Value.AsDouble() <= value2.Value.AsDouble()),
                                    ILOpcode.bge_s => value1.Value.AsDouble() >= value2.Value.AsDouble(),
                                    ILOpcode.bge_un_s => !(value1.Value.AsDouble() < value2.Value.AsDouble()),
                                    ILOpcode.beq_s => value1.Value.AsDouble() == value2.Value.AsDouble(),
                                    ILOpcode.bne_un_s => value1.Value.AsDouble() != value2.Value.AsDouble(),
                                    ILOpcode.ble_s => value1.Value.AsDouble() <= value2.Value.AsDouble(),
                                    ILOpcode.ble_un_s => !(value1.Value.AsDouble() > value2.Value.AsDouble()),
                                    _ => throw new NotImplementedException() // unreachable
                                };
                            }
                            else
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode);
                            }
                        }
                        else
                        {
                            Debug.Assert(normalizedOpcode == ILOpcode.br_s);
                            branchTaken = true;
                        }

                        if (branchTaken)
                        {
                            reader.Seek(target);
                        }
                    }
                    break;

                    case ILOpcode.switch_:
                    {
                        StackEntry val = stack.Pop();
                        if (val.ValueKind is not StackValueKind.Int32)
                            ThrowHelper.ThrowInvalidProgramException();

                        uint target = (uint)val.Value.AsInt32();

                        uint count = reader.ReadILUInt32();
                        int nextInstruction = reader.Offset + (int)(4 * count);
                        if (target > count)
                        {
                            reader.Seek(nextInstruction);
                        }
                        else
                        {
                            reader.Seek(reader.Offset + (int)(4 * target));
                            reader.Seek(nextInstruction + (int)reader.ReadILUInt32());
                        }
                    }
                    break;

                    case ILOpcode.leave:
                    case ILOpcode.leave_s:
                    {
                        stack.Clear();

                        // We assume no finally regions (would have to run them here)
                        // This is validated before, but we're being paranoid.
                        foreach (ILExceptionRegion ehRegion in ehRegions)
                        {
                            Debug.Assert(ehRegion.Kind != ILExceptionRegionKind.Finally);
                        }

                        int delta = opcode == ILOpcode.leave ?
                            (int)reader.ReadILUInt32() :
                            (sbyte)reader.ReadILByte();
                        int target = reader.Offset + delta;
                        if (target < 0
                            || target > reader.Size)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        reader.Seek(target);
                    }
                    break;

                    case ILOpcode.clt:
                    case ILOpcode.clt_un:
                    case ILOpcode.cgt:
                    case ILOpcode.cgt_un:
                    {
                        StackEntry value1 = stack.Pop();
                        StackEntry value2 = stack.Pop();

                        bool condition;
                        if (value1.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32 && value2.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32)
                        {
                            if (opcode == ILOpcode.cgt)
                                condition = value1.Value.AsInt32() < value2.Value.AsInt32();
                            else if (opcode == ILOpcode.cgt_un)
                                condition = (uint)value1.Value.AsInt32() < (uint)value2.Value.AsInt32();
                            else if (opcode == ILOpcode.clt)
                                condition = value1.Value.AsInt32() > value2.Value.AsInt32();
                            else if (opcode == ILOpcode.clt_un)
                                condition = (uint)value1.Value.AsInt32() > (uint)value2.Value.AsInt32();
                            else
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else if (value1.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64 && value2.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64)
                        {
                            if (opcode == ILOpcode.cgt)
                                condition = value1.Value.AsInt64() < value2.Value.AsInt64();
                            else if (opcode == ILOpcode.cgt_un)
                                condition = (ulong)value1.Value.AsInt64() < (ulong)value2.Value.AsInt64();
                            else if (opcode == ILOpcode.clt)
                                condition = value1.Value.AsInt64() > value2.Value.AsInt64();
                            else if (opcode == ILOpcode.clt_un)
                                condition = (ulong)value1.Value.AsInt64() > (ulong)value2.Value.AsInt64();
                            else
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else if (value1.ValueKind == StackValueKind.Float && value2.ValueKind == StackValueKind.Float)
                        {
                            if (opcode == ILOpcode.cgt)
                                condition = value1.Value.AsDouble() < value2.Value.AsDouble();
                            else if (opcode == ILOpcode.cgt_un)
                                condition = !(value1.Value.AsDouble() >= value2.Value.AsDouble());
                            else if (opcode == ILOpcode.clt)
                                condition = value1.Value.AsDouble() > value2.Value.AsDouble();
                            else if (opcode == ILOpcode.clt_un)
                                condition = !(value1.Value.AsDouble() <= value2.Value.AsDouble());
                            else
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else if (value1.ValueKind == StackValueKind.ObjRef && value2.ValueKind == StackValueKind.ObjRef)
                        {
                            if (opcode == ILOpcode.cgt_un)
                                condition = value1.Value == null && value2.Value != null;
                            else
                                return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        stack.Push(StackValueKind.Int32, condition
                                ? ValueTypeValue.FromInt32(1)
                                : ValueTypeValue.FromInt32(0));
                    }
                    break;

                    case ILOpcode.ceq:
                    {
                        StackEntry value1 = stack.Pop();
                        StackEntry value2 = stack.Pop();

                        if (value1.ValueKind == value2.ValueKind
                            && Value.TryCompareEquality(value1.Value, value2.Value, out bool compareResult))
                        {
                            stack.Push(StackValueKind.Int32,
                                compareResult
                                ? ValueTypeValue.FromInt32(1)
                                : ValueTypeValue.FromInt32(0));
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                    }
                    break;

                    case ILOpcode.neg:
                    {
                        StackEntry value = stack.Pop();
                        if (value.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32)
                            stack.Push(value.ValueKind, ValueTypeValue.FromInt32(-value.Value.AsInt32()));
                        else if (value.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64)
                            stack.Push(value.ValueKind, ValueTypeValue.FromInt64(-value.Value.AsInt64()));
                        else if (value.ValueKind == StackValueKind.Float)
                            stack.Push(value.ValueKind, ValueTypeValue.FromDouble(-value.Value.AsDouble()));
                        else
                            return Status.Fail(methodIL.OwningMethod, opcode);
                    }
                    break;

                    case ILOpcode.or:
                    case ILOpcode.shl:
                    case ILOpcode.add:
                    case ILOpcode.sub:
                    case ILOpcode.mul:
                    case ILOpcode.and:
                    case ILOpcode.div:
                    case ILOpcode.div_un:
                    case ILOpcode.rem:
                    case ILOpcode.rem_un:
                    {
                        bool isDivRem = opcode == ILOpcode.div || opcode == ILOpcode.div_un
                            || opcode == ILOpcode.rem || opcode == ILOpcode.rem_un;

                        StackEntry value2 = stack.Pop();
                        StackEntry value1 = stack.Pop();

                        bool isNint = value1.ValueKind == StackValueKind.NativeInt || value2.ValueKind == StackValueKind.NativeInt;

                        if (value1.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32 && value2.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int32)
                        {
                            if (isDivRem && value2.Value.AsInt32() == 0)
                                return Status.Fail(methodIL.OwningMethod, opcode, "Division by zero");

                            int result = opcode switch
                            {
                                ILOpcode.or => value1.Value.AsInt32() | value2.Value.AsInt32(),
                                ILOpcode.shl => value1.Value.AsInt32() << value2.Value.AsInt32(),
                                ILOpcode.add => value1.Value.AsInt32() + value2.Value.AsInt32(),
                                ILOpcode.sub => value1.Value.AsInt32() - value2.Value.AsInt32(),
                                ILOpcode.and => value1.Value.AsInt32() & value2.Value.AsInt32(),
                                ILOpcode.mul => value1.Value.AsInt32() * value2.Value.AsInt32(),
                                ILOpcode.div => value1.Value.AsInt32() / value2.Value.AsInt32(),
                                ILOpcode.div_un => (int)((uint)value1.Value.AsInt32() / (uint)value2.Value.AsInt32()),
                                ILOpcode.rem => value1.Value.AsInt32() % value2.Value.AsInt32(),
                                ILOpcode.rem_un => (int)((uint)value1.Value.AsInt32() % (uint)value2.Value.AsInt32()),
                                _ => throw new NotImplementedException(), // unreachable
                            };

                            stack.Push(isNint ? StackValueKind.NativeInt : StackValueKind.Int32, ValueTypeValue.FromInt32(result));
                        }
                        else if (value1.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64 && value2.ValueKind.WithNormalizedNativeInt(context) == StackValueKind.Int64)
                        {
                            if (isDivRem && value2.Value.AsInt64() == 0)
                                return Status.Fail(methodIL.OwningMethod, opcode, "Division by zero");

                            long result = opcode switch
                            {
                                ILOpcode.or => value1.Value.AsInt64() | value2.Value.AsInt64(),
                                ILOpcode.add => value1.Value.AsInt64() + value2.Value.AsInt64(),
                                ILOpcode.sub => value1.Value.AsInt64() - value2.Value.AsInt64(),
                                ILOpcode.and => value1.Value.AsInt64() & value2.Value.AsInt64(),
                                ILOpcode.mul => value1.Value.AsInt64() * value2.Value.AsInt64(),
                                ILOpcode.div => value1.Value.AsInt64() / value2.Value.AsInt64(),
                                ILOpcode.div_un => (long)((ulong)value1.Value.AsInt64() / (ulong)value2.Value.AsInt64()),
                                ILOpcode.rem => value1.Value.AsInt64() % value2.Value.AsInt64(),
                                ILOpcode.rem_un => (long)((ulong)value1.Value.AsInt64() % (ulong)value2.Value.AsInt64()),
                                _ => throw new NotImplementedException(), // unreachable
                            };

                            stack.Push(isNint ? StackValueKind.NativeInt : StackValueKind.Int64, ValueTypeValue.FromInt64(result));
                        }
                        else if (value1.ValueKind == StackValueKind.Float && value2.ValueKind == StackValueKind.Float)
                        {
                            if (isDivRem && value2.Value.AsDouble() == 0)
                                return Status.Fail(methodIL.OwningMethod, opcode, "Division by zero");

                            if (opcode == ILOpcode.or || opcode == ILOpcode.shl || opcode == ILOpcode.and || opcode == ILOpcode.div_un || opcode == ILOpcode.rem_un)
                                ThrowHelper.ThrowInvalidProgramException();

                            double result = opcode switch
                            {
                                ILOpcode.add => value1.Value.AsDouble() + value2.Value.AsDouble(),
                                ILOpcode.sub => value1.Value.AsDouble() - value2.Value.AsDouble(),
                                ILOpcode.mul => value1.Value.AsDouble() * value2.Value.AsDouble(),
                                ILOpcode.div => value1.Value.AsDouble() / value2.Value.AsDouble(),
                                ILOpcode.rem => value1.Value.AsDouble() % value2.Value.AsDouble(),
                                _ => throw new NotImplementedException(), // unreachable
                            };

                            stack.Push(StackValueKind.Float, ValueTypeValue.FromDouble(result));
                        }
                        else if (value1.ValueKind == StackValueKind.Int64 && value2.ValueKind == StackValueKind.Int32
                            && opcode == ILOpcode.shl)
                        {
                            long result = value1.Value.AsInt64() << value2.Value.AsInt32();
                            stack.Push(isNint ? StackValueKind.NativeInt : StackValueKind.Int64, ValueTypeValue.FromInt64(result));
                        }
                        else if ((value1.ValueKind == StackValueKind.ByRef && value2.ValueKind != StackValueKind.ByRef)
                            || (value2.ValueKind == StackValueKind.ByRef && value1.ValueKind != StackValueKind.ByRef))
                        {
                            if (opcode != ILOpcode.add)
                                ThrowHelper.ThrowInvalidProgramException();

                            StackEntry reference = value1.ValueKind == StackValueKind.ByRef ? value1 : value2;
                            StackEntry addend = value1.ValueKind != StackValueKind.ByRef ? value1 : value2;

                            if (addend.ValueKind is not StackValueKind.NativeInt and not StackValueKind.Int32)
                                ThrowHelper.ThrowInvalidProgramException();

                            long addition = addend.ValueKind switch
                            {
                                StackValueKind.Int32 => addend.Value.AsInt32(),
                                _ => context.Target.PointerSize == 8 ? addend.Value.AsInt64() : addend.Value.AsInt32()
                            };

                            if (reference.Value is not ByRefValue previousByRef)
                                return Status.Fail(methodIL.OwningMethod, "Byref math with unsupported byref");

                            if (addition > previousByRef.PointedToBytes.Length - previousByRef.PointedToOffset
                                || addition + previousByRef.PointedToOffset < 0)
                                return Status.Fail(methodIL.OwningMethod, "Out of range byref access");

                            stack.Push(StackValueKind.ByRef, new ByRefValue(previousByRef.PointedToBytes, (int)(previousByRef.PointedToOffset + addition)));
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }
                    }
                    break;

                    case ILOpcode.ldlen:
                    {
                        StackEntry popped = stack.Pop();
                        if (popped.Value is ArrayInstance arrayInstance)
                        {
                            stack.Push(StackValueKind.NativeInt, context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64(arrayInstance.Length) : ValueTypeValue.FromInt32(arrayInstance.Length));
                        }
                        else if (popped.Value == null)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Null array");
                        }
                        else
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                    }
                    break;

                    case ILOpcode.stelem:
                    case ILOpcode.stelem_i:
                    case ILOpcode.stelem_i1:
                    case ILOpcode.stelem_i2:
                    case ILOpcode.stelem_i4:
                    case ILOpcode.stelem_i8:
                    case ILOpcode.stelem_r4:
                    case ILOpcode.stelem_r8:
                    {
                        TypeDesc elementType = opcode switch
                        {
                            ILOpcode.stelem_i => context.GetWellKnownType(WellKnownType.IntPtr),
                            ILOpcode.stelem_i1 => context.GetWellKnownType(WellKnownType.SByte),
                            ILOpcode.stelem_i2 => context.GetWellKnownType(WellKnownType.Int16),
                            ILOpcode.stelem_i4 => context.GetWellKnownType(WellKnownType.Int32),
                            ILOpcode.stelem_i8 => context.GetWellKnownType(WellKnownType.Int64),
                            ILOpcode.stelem_r4 => context.GetWellKnownType(WellKnownType.Single),
                            ILOpcode.stelem_r8 => context.GetWellKnownType(WellKnownType.Double),
                            _ => (TypeDesc)methodIL.GetObject(reader.ReadILToken()),
                        };

                        if (elementType.IsGCPointer)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        Value value = stack.PopIntoLocation(elementType);
                        if (!stack.TryPopIntValue(out int index))
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        StackEntry array = stack.Pop();
                        if (array.Value is ArrayInstance arrayInstance)
                        {
                            if (!arrayInstance.TryStoreElement(index, value))
                                return Status.Fail(methodIL.OwningMethod, opcode, "Out of range access");
                        }
                        else if (array.Value == null)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Null array");
                        }
                        else
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                    }
                    break;

                    case ILOpcode.ldelem:
                    case ILOpcode.ldelem_i:
                    case ILOpcode.ldelem_i1:
                    case ILOpcode.ldelem_u1:
                    case ILOpcode.ldelem_i2:
                    case ILOpcode.ldelem_u2:
                    case ILOpcode.ldelem_i4:
                    case ILOpcode.ldelem_u4:
                    case ILOpcode.ldelem_i8:
                    case ILOpcode.ldelem_r4:
                    case ILOpcode.ldelem_r8:
                    {
                        TypeDesc elementType = opcode switch
                        {
                            ILOpcode.ldelem_i => context.GetWellKnownType(WellKnownType.IntPtr),
                            ILOpcode.ldelem_i1 => context.GetWellKnownType(WellKnownType.SByte),
                            ILOpcode.ldelem_u1 => context.GetWellKnownType(WellKnownType.Byte),
                            ILOpcode.ldelem_i2 => context.GetWellKnownType(WellKnownType.Int16),
                            ILOpcode.ldelem_u2 => context.GetWellKnownType(WellKnownType.UInt16),
                            ILOpcode.ldelem_i4 => context.GetWellKnownType(WellKnownType.Int32),
                            ILOpcode.ldelem_u4 => context.GetWellKnownType(WellKnownType.UInt32),
                            ILOpcode.ldelem_i8 => context.GetWellKnownType(WellKnownType.Int64),
                            ILOpcode.ldelem_r4 => context.GetWellKnownType(WellKnownType.Single),
                            ILOpcode.ldelem_r8 => context.GetWellKnownType(WellKnownType.Double),
                            _ => (TypeDesc)methodIL.GetObject(reader.ReadILToken()),
                        };

                        if (elementType.IsGCPointer)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode);
                        }

                        if (!stack.TryPopIntValue(out int index))
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                        StackEntry array = stack.Pop();
                        if (array.Value is ArrayInstance arrayInstance)
                        {
                            if (!arrayInstance.TryLoadElement(index, out Value value))
                                return Status.Fail(methodIL.OwningMethod, opcode, "Out of range access");

                            stack.PushFromLocation(elementType, value);
                        }
                        else if (array.Value == null)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Null array");
                        }
                        else if (array.Value is ForeignTypeInstance)
                        {
                            return Status.Fail(methodIL.OwningMethod, opcode, "Foreign array");
                        }
                        else
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }

                    }
                    break;

                    case ILOpcode.box:
                    {
                        TypeDesc type = (TypeDesc)methodIL.GetObject(reader.ReadILToken());
                        if (type.IsValueType)
                        {
                            if (type.IsNullable)
                                return Status.Fail(methodIL.OwningMethod, opcode);

                            if (type.RequiresAlign8())
                                return Status.Fail(methodIL.OwningMethod, opcode, "Align8");

                            Value value = stack.PopIntoLocation(type);
                            AllocationSite allocSite = new AllocationSite(_type, instructionCounter);
                            if (!ObjectInstance.TryBox((DefType)type, value, allocSite, out ObjectInstance boxedResult))
                            {
                                return Status.Fail(methodIL.OwningMethod, opcode);
                            }


                            stack.Push(boxedResult);
                        }
                    }
                    break;

                    case ILOpcode.unbox_any:
                    {
                        TypeDesc type = (TypeDesc)methodIL.GetObject(reader.ReadILToken());
                        StackEntry entry = stack.Pop();
                        if (entry.Value is ObjectInstance objInst
                            && objInst.TryUnboxAny(type, out Value unboxed))
                        {
                            stack.PushFromLocation(type, unboxed);
                        }
                        else
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                    }
                    break;

                    case ILOpcode.ldobj:
                    case ILOpcode.ldind_i1:
                    case ILOpcode.ldind_u1:
                    case ILOpcode.ldind_i2:
                    case ILOpcode.ldind_u2:
                    case ILOpcode.ldind_i4:
                    case ILOpcode.ldind_u4:
                    case ILOpcode.ldind_i8:
                    {
                        TypeDesc type = opcode switch
                        {
                            ILOpcode.ldind_i1 => context.GetWellKnownType(WellKnownType.SByte),
                            ILOpcode.ldind_u1 => context.GetWellKnownType(WellKnownType.Byte),
                            ILOpcode.ldind_i2 => context.GetWellKnownType(WellKnownType.Int16),
                            ILOpcode.ldind_u2 => context.GetWellKnownType(WellKnownType.UInt16),
                            ILOpcode.ldind_i4 => context.GetWellKnownType(WellKnownType.Int32),
                            ILOpcode.ldind_u4 => context.GetWellKnownType(WellKnownType.UInt32),
                            ILOpcode.ldind_i8 => context.GetWellKnownType(WellKnownType.Int64),
                            _ /* ldobj */ => (TypeDesc)methodIL.GetObject(reader.ReadILToken()),
                        };

                        StackEntry entry = stack.Pop();
                        if (entry.ValueKind != StackValueKind.ByRef && entry.ValueKind != StackValueKind.NativeInt)
                            ThrowHelper.ThrowInvalidProgramException();

                        if (entry.Value is ByRefValueBase byRefVal
                            && byRefVal.TryLoad(type, out Value dereferenced))
                        {
                            stack.PushFromLocation(type, dereferenced);
                        }
                        else
                        {
                            return Status.Fail(methodIL.OwningMethod, "Ldind from unsupported byref");
                        }
                    }
                    break;

                    case ILOpcode.stobj:
                    case ILOpcode.stind_i:
                    case ILOpcode.stind_i1:
                    case ILOpcode.stind_i2:
                    case ILOpcode.stind_i4:
                    case ILOpcode.stind_i8:
                    {
                        if (opcode == ILOpcode.stobj)
                        {
                            TypeDesc type = methodIL.GetObject(reader.ReadILToken()) as TypeDesc;
                            opcode = type.Category switch
                            {
                                TypeFlags.SByte or TypeFlags.Boolean or TypeFlags.Byte => ILOpcode.stind_i1,
                                TypeFlags.Int16 or TypeFlags.Char or TypeFlags.UInt16 => ILOpcode.stind_i2,
                                TypeFlags.Int32 or TypeFlags.UInt32 => ILOpcode.stind_i4,
                                TypeFlags.Int64 or TypeFlags.UInt64 => ILOpcode.stind_i8,
                                TypeFlags.IntPtr or TypeFlags.UIntPtr => ILOpcode.stind_i,
                                _ => ILOpcode.stobj,
                            };
                        }

                        Value val = opcode switch
                        {
                            ILOpcode.stind_i1 => stack.PopIntoLocation(context.GetWellKnownType(WellKnownType.Byte)),
                            ILOpcode.stind_i2 => stack.PopIntoLocation(context.GetWellKnownType(WellKnownType.UInt16)),
                            ILOpcode.stind_i4 => stack.PopIntoLocation(context.GetWellKnownType(WellKnownType.UInt32)),
                            ILOpcode.stind_i8 => stack.PopIntoLocation(context.GetWellKnownType(WellKnownType.UInt64)),
                            ILOpcode.stind_i => stack.PopIntoLocation(context.GetWellKnownType(WellKnownType.UIntPtr)),
                            _ => stack.Pop().Value
                        };

                        StackEntry location = stack.Pop();
                        if (location.ValueKind != StackValueKind.ByRef && location.ValueKind != StackValueKind.NativeInt)
                            ThrowHelper.ThrowInvalidProgramException();

                        if (location.Value is not ByRefValueBase destValue)
                            return Status.Fail(methodIL.OwningMethod, "Stind into usupported byref");

                        if (!destValue.TryStore(val))
                            return Status.Fail(methodIL.OwningMethod, "Byref doesn't support storing value");
                    }
                    break;

                    case ILOpcode.constrained:
                        constrainedType = methodIL.GetObject(reader.ReadILToken()) as TypeDesc;
                        goto again;

                    case ILOpcode.unaligned:
                        reader.ReadILByte();
                        break;

                    case ILOpcode.initblk:
                    {
                        StackEntry size = stack.Pop();
                        StackEntry value = stack.Pop();
                        StackEntry addr = stack.Pop();

                        if (size.ValueKind != StackValueKind.Int32
                            || value.ValueKind != StackValueKind.Int32
                            || addr.ValueKind != StackValueKind.ByRef)
                            return Status.Fail(methodIL.OwningMethod, opcode);

                        uint sizeBytes = (uint)size.Value.AsInt32();

                        if (addr.Value is not ByRefValue addressValue)
                            return Status.Fail(methodIL.OwningMethod, "initblk of unsupported byref");

                        if (sizeBytes > addressValue.PointedToBytes.Length - addressValue.PointedToOffset
                            || sizeBytes > int.MaxValue /* paranoid check that cast to int is legit */)
                            return Status.Fail(methodIL.OwningMethod, opcode);

                        Array.Fill(addressValue.PointedToBytes, (byte)value.Value.AsInt32(), addressValue.PointedToOffset, (int)sizeBytes);
                    }
                    break;

                    default:
                        return Status.Fail(methodIL.OwningMethod, opcode);
                }

            }

            return Status.Fail(methodIL.OwningMethod, "Control fell through");
        }

        private static bool TryGetSpanElementType(TypeDesc type, bool isReadOnlySpan, out MetadataType elementType)
        {
            if (type.IsByRefLike
                && type is MetadataType maybeSpan
                && maybeSpan.Module == type.Context.SystemModule
                && ((isReadOnlySpan && maybeSpan.Name == "ReadOnlySpan`1") || (!isReadOnlySpan && maybeSpan.Name == "Span`1"))
                && maybeSpan.Namespace == "System"
                && maybeSpan.Instantiation[0] is MetadataType readOnlySpanElementType)
            {
                elementType = readOnlySpanElementType;
                return true;
            }
            elementType = null;
            return false;
        }

        private static BaseValueTypeValue NewUninitializedLocationValue(TypeDesc locationType, FieldDesc fieldThatOwnsMemory)
        {
            if (locationType.IsGCPointer || locationType.IsByRef)
            {
                return null;
            }
            else if (TryGetSpanElementType(locationType, isReadOnlySpan: true, out MetadataType readOnlySpanElementType))
            {
                return new SpanValue(readOnlySpanElementType, Array.Empty<byte>(), 0, 0);
            }
            else if (TryGetSpanElementType(locationType, isReadOnlySpan: false, out MetadataType spanElementType))
            {
                return new SpanValue(spanElementType, Array.Empty<byte>(), 0, 0);
            }
            else if (VTableLikeStructValue.IsCompatible(locationType))
            {
                return new VTableLikeStructValue((MetadataType)locationType, fieldThatOwnsMemory);
            }
            else if (ComInterfaceEntryArrayValue.IsCompatible(locationType, out TypeDesc comInterfaceEntryType))
            {
                return new ComInterfaceEntryArrayValue(locationType, comInterfaceEntryType);
            }
            else
            {
                Debug.Assert(locationType.IsValueType || locationType.IsPointer || locationType.IsFunctionPointer);
                return new ValueTypeValue(locationType);
            }
        }

        private bool TryHandleIntrinsicCall(MethodDesc method, Value[] parameters, out Value retVal)
        {
            retVal = default;

            switch (method.Name)
            {
                case "InitializeArray":
                    if (method.OwningType is MetadataType mdType
                        && mdType.Name == "RuntimeHelpers" && mdType.Namespace == "System.Runtime.CompilerServices"
                        && mdType.Module == mdType.Context.SystemModule
                        && parameters[0] is ArrayInstance array
                        && parameters[1] is RuntimeFieldHandleValue fieldHandle
                        && fieldHandle.Field.IsStatic && fieldHandle.Field.HasRva
                        && fieldHandle.Field is Internal.TypeSystem.Ecma.EcmaField ecmaField)
                    {
                        byte[] rvaData = Internal.TypeSystem.Ecma.EcmaFieldExtensions.GetFieldRvaData(ecmaField);
                        return array.TryInitialize(rvaData);
                    }
                    return false;
                case "CreateSpan":
                    if (method.OwningType is MetadataType createSpanType
                        && createSpanType.Name == "RuntimeHelpers" && createSpanType.Namespace == "System.Runtime.CompilerServices"
                        && createSpanType.Module == createSpanType.Context.SystemModule
                        && parameters[0] is RuntimeFieldHandleValue createSpanFieldHandle
                        && createSpanFieldHandle.Field.IsStatic && createSpanFieldHandle.Field.HasRva
                        && createSpanFieldHandle.Field is Internal.TypeSystem.Ecma.EcmaField createSpanEcmaField
                        && method.Instantiation[0].IsValueType)
                    {
                        var elementType = (MetadataType)method.Instantiation[0];
                        int elementSize = elementType.InstanceFieldSize.AsInt;
                        byte[] rvaData = Internal.TypeSystem.Ecma.EcmaFieldExtensions.GetFieldRvaData(createSpanEcmaField);
                        if (rvaData.Length % elementSize != 0)
                            return false;
                        retVal = new SpanValue(elementType, rvaData, 0, rvaData.Length);
                        return true;
                    }
                    return false;
                case "GetTypeFromHandle" when IsSystemType(method.OwningType)
                        && parameters[0] is RuntimeTypeHandleValue typeHandle:
                {
                    if (!_internedTypes.TryGetValue(typeHandle.Type, out RuntimeTypeValue runtimeType))
                    {
                        _internedTypes.Add(typeHandle.Type, runtimeType = new RuntimeTypeValue(typeHandle.Type));
                    }
                    retVal = runtimeType;
                    return true;
                }
                case "get_IsValueType" when IsSystemType(method.OwningType)
                        && parameters[0] is RuntimeTypeValue typeToCheckForValueType:
                {
                    retVal = ValueTypeValue.FromSByte(typeToCheckForValueType.TypeRepresented.IsValueType ? (sbyte)1 : (sbyte)0);
                    return true;
                }
                case "op_Equality" when IsSystemType(method.OwningType)
                        && (parameters[0] is RuntimeTypeValue || parameters[1] is RuntimeTypeValue):
                {
                    retVal = ValueTypeValue.FromSByte(parameters[0] == parameters[1] ? (sbyte)1 : (sbyte)0);
                    return true;
                }
                case "IsReferenceOrContainsReferences" when method.Instantiation.Length == 1
                        && method.OwningType is MetadataType isReferenceOrContainsReferencesType
                        && isReferenceOrContainsReferencesType.Name == "RuntimeHelpers" && isReferenceOrContainsReferencesType.Namespace == "System.Runtime.CompilerServices"
                        && isReferenceOrContainsReferencesType.Module == method.Context.SystemModule:
                {
                    bool result = method.Instantiation[0].IsGCPointer || (method.Instantiation[0] is DefType defType && defType.ContainsGCPointers);
                    retVal = ValueTypeValue.FromSByte(result ? (sbyte)1 : (sbyte)0);
                    return true;
                }
                case "GetArrayDataReference" when method.Instantiation.Length == 1
                        && method.OwningType is MetadataType getArrayDataReferenceType
                        && getArrayDataReferenceType.Name == "MemoryMarshal" && getArrayDataReferenceType.Namespace == "System.Runtime.InteropServices"
                        && getArrayDataReferenceType.Module == method.Context.SystemModule
                        && parameters[0] is ArrayInstance arrayData
                        && ((ArrayType)arrayData.Type).ElementType == method.Instantiation[0]:
                {
                    retVal = arrayData.GetArrayData();
                    return true;
                }
            }

            static bool IsSystemType(TypeDesc type)
                => type is MetadataType typeType
                        && typeType.Name == "Type" && typeType.Namespace == "System"
                        && typeType.Module == typeType.Context.SystemModule;

            return false;
        }

        private static TypeDesc GetArgType(MethodDesc method, int index)
        {
            var sig = method.Signature;
            int offset = 0;
            if (!sig.IsStatic)
            {
                if (index == 0)
                    return method.OwningType.IsValueType ? method.OwningType.MakeByRefType() : method.OwningType;
                offset = 1;
            }

            if ((uint)(index - offset) >= (uint)sig.Length)
                ThrowHelper.ThrowInvalidProgramException();

            return sig[index - offset];
        }

        private sealed class Stack : Stack<StackEntry>
        {
            private readonly TargetDetails _target;

            public Stack(int capacity, TargetDetails target) : base(capacity)
            {
                _target = target;
            }

            public new StackEntry Pop()
            {
                if (Count < 1)
                {
                    ThrowHelper.ThrowInvalidProgramException();
                }

                return base.Pop();
            }

            public bool TryPopIntValue(out int value)
            {
                if (Count == 0)
                {
                    value = 0;
                    return false;
                }

                StackEntry entry = Pop();
                if (entry.ValueKind == StackValueKind.Int32)
                {
                    value = entry.Value.AsInt32();
                    return true;
                }
                else if (entry.ValueKind == StackValueKind.NativeInt)
                {
                    if (_target.PointerSize == 8)
                    {
                        long longValue = entry.Value.AsInt64();
                        if (longValue < int.MinValue || longValue > int.MaxValue)
                        {
                            value = 0;
                            return false;
                        }
                        value = (int)longValue;
                        return true;
                    }

                    value = entry.Value.AsInt32();
                    return true;
                }

                value = 0;
                return false;
            }

            public void Push(StackValueKind kind, Value val)
            {
                Push(new StackEntry(kind, val));
            }

            public void Push(ReferenceTypeValue value)
            {
                Push(StackValueKind.ObjRef, value);
            }

            public void PushFromLocation(TypeDesc locationType, Value value)
            {
                switch (locationType.UnderlyingType.Category)
                {
                    case TypeFlags.Boolean:
                    case TypeFlags.Byte:
                        Push(StackValueKind.Int32, ValueTypeValue.FromInt32((byte)value.AsSByte())); break;
                    case TypeFlags.Char:
                    case TypeFlags.UInt16:
                        Push(StackValueKind.Int32, ValueTypeValue.FromInt32((ushort)value.AsInt16())); break;
                    case TypeFlags.SByte:
                        Push(StackValueKind.Int32, ValueTypeValue.FromInt32(value.AsSByte())); break;
                    case TypeFlags.Int16:
                        Push(StackValueKind.Int32, ValueTypeValue.FromInt32(value.AsInt16())); break;
                    case TypeFlags.Int32:
                    case TypeFlags.UInt32:
                        Push(StackValueKind.Int32, value.Clone()); break;
                    case TypeFlags.Int64:
                    case TypeFlags.UInt64:
                        Push(StackValueKind.Int64, value.Clone()); break;
                    case TypeFlags.IntPtr:
                    case TypeFlags.UIntPtr:
                    case TypeFlags.Pointer:
                    case TypeFlags.FunctionPointer:
                        Push(StackValueKind.NativeInt, value.Clone()); break;
                    case TypeFlags.Single:
                        Push(StackValueKind.Float, ValueTypeValue.FromDouble(value.AsSingle())); break;
                    case TypeFlags.Double:
                        Push(StackValueKind.Float, value.Clone()); break;
                    case TypeFlags.ValueType:
                    case TypeFlags.Nullable:
                        Push(StackValueKind.ValueType, value.Clone()); break;
                    case TypeFlags.Class:
                    case TypeFlags.Interface:
                    case TypeFlags.Array:
                    case TypeFlags.SzArray:
                        Push(StackValueKind.ObjRef, value); break;
                    case TypeFlags.ByRef:
                        Push(StackValueKind.ByRef, value); break;
                    default:
                        throw new NotImplementedException();
                }
            }

            public Value PopIntoLocation(TypeDesc locationType)
            {
                if (Count == 0)
                {
                    ThrowHelper.ThrowInvalidProgramException();
                }

                locationType = locationType.UnderlyingType;

                StackEntry popped = Pop();

                switch (popped.ValueKind)
                {
                    case StackValueKind.Int64:
                        if (!locationType.IsWellKnownType(WellKnownType.Int64)
                            && !locationType.IsWellKnownType(WellKnownType.UInt64))
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return popped.Value;

                    case StackValueKind.Int32:
                        if (!locationType.IsWellKnownType(WellKnownType.Int32)
                            && !locationType.IsWellKnownType(WellKnownType.UInt32))
                        {
                            int value = popped.Value.AsInt32();
                            switch (locationType.Category)
                            {
                                case TypeFlags.SByte:
                                case TypeFlags.Byte:
                                case TypeFlags.Boolean:
                                    return ValueTypeValue.FromSByte((sbyte)value);
                                case TypeFlags.Int16:
                                case TypeFlags.UInt16:
                                case TypeFlags.Char:
                                    return ValueTypeValue.FromInt16((short)value);
                                // case TypeFlags.IntPtr: sign extend
                                // case TypeFlags.UIntPtr: zero extend
                            }
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return popped.Value;

                    case StackValueKind.NativeInt:
                        // True byref that we converted to nint at some point.
                        if (locationType.IsByRef && popped.Value is ByRefValueBase)
                            return popped.Value;

                        // If it's none of the natural pointer types, we might need to truncate.
                        if (!locationType.IsPointer
                            && !locationType.IsFunctionPointer
                            && !locationType.IsWellKnownType(WellKnownType.IntPtr)
                            && !locationType.IsWellKnownType(WellKnownType.UIntPtr))
                        {
                            long value = _target.PointerSize == 8 ? popped.Value.AsInt64() : popped.Value.AsInt32();
                            switch (locationType.Category)
                            {
                                case TypeFlags.SByte:
                                case TypeFlags.Byte:
                                case TypeFlags.Boolean:
                                    return ValueTypeValue.FromSByte((sbyte)value);
                                case TypeFlags.Int16:
                                case TypeFlags.UInt16:
                                case TypeFlags.Char:
                                    return ValueTypeValue.FromInt16((short)value);
                                case TypeFlags.Int32:
                                case TypeFlags.UInt32:
                                    return ValueTypeValue.FromInt32((int)value);
                                // case TypeFlags.ByRef: start GC tracking
                            }

                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return popped.Value;

                    case StackValueKind.Float:
                        if (locationType.IsWellKnownType(WellKnownType.Double))
                        {
                            return popped.Value;
                        }
                        else if (!locationType.IsWellKnownType(WellKnownType.Single))
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return ValueTypeValue.FromSingle((float)popped.Value.AsDouble());

                    case StackValueKind.ByRef:
                        if (!locationType.IsByRef
                            && locationType.Category is not TypeFlags.IntPtr and not TypeFlags.UIntPtr and not TypeFlags.Pointer and not TypeFlags.FunctionPointer)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return popped.Value;

                    case StackValueKind.ObjRef:
                        if (!locationType.IsGCPointer)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return popped.Value;

                    case StackValueKind.ValueType:
                        if (!locationType.IsValueType
                            || ((BaseValueTypeValue)popped.Value).Size != ((DefType)locationType).InstanceFieldSize.AsInt)
                        {
                            ThrowHelper.ThrowInvalidProgramException();
                        }
                        return popped.Value;

                    default:
                        throw new NotImplementedException();
                }
            }
        }

        /// <summary>
        /// Represents a field value that can be serialized into a preinitialized blob.
        /// </summary>
        public interface ISerializableValue
        {
            void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory);

            bool GetRawData(NodeFactory factory, out object data);
        }

        /// <summary>
        /// Represents a frozen object whose contents can be serialized into the executable.
        /// </summary>
        public interface ISerializableReference : ISerializableValue
        {
            TypeDesc Type { get; }
            void WriteContent(ref ObjectDataBuilder builder, ISymbolNode thisNode, NodeFactory factory);
            bool HasConditionalDependencies { get; }
            void GetConditionalDependencies(ref CombinedDependencyList dependencies, NodeFactory factory);
            bool IsKnownImmutable { get; }
            int ArrayLength { get; }
        }

        /// <summary>
        /// Represents a value with instance fields. This is either a reference type, or a byref to
        /// a valuetype.
        /// </summary>
        private interface IHasInstanceFields
        {
            bool TrySetField(FieldDesc field, Value value);
            Value GetField(FieldDesc field);
            ByRefValueBase GetFieldAddress(FieldDesc field);
        }

        /// <summary>
        /// Represents a special value that is used internally to model known constructs, but cannot
        /// be represented externally and that's why we don't allow field stores with it.
        /// </summary>
        private interface IInternalModelingOnlyValue
        {
        }

        private interface INativeIntConvertibleValue;

        /// <summary>
        /// Represents a value that can be assigned into.
        /// </summary>
        private interface IAssignableValue
        {
            bool TryAssign(Value value);
        }

        private abstract class Value : ISerializableValue
        {
            public abstract bool TryCompareEquality(Value value, out bool result);

            public static bool TryCompareEquality(Value value1, Value value2, out bool result)
            {
                if (value1 == value2)
                {
                    result = true;
                    return true;
                }
                if (value1 == null || value2 == null)
                {
                    result = false;
                    return true;
                }
                return value1.TryCompareEquality(value2, out result);
            }

            public virtual bool TryCreateByRef(out Value value)
            {
                value = null;
                return false;
            }

            public abstract void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory);

            public abstract bool GetRawData(NodeFactory factory, out object data);

            private static T ThrowInvalidProgram<T>()
            {
                ThrowHelper.ThrowInvalidProgramException();
                return default;
            }

            public virtual sbyte AsSByte() => ThrowInvalidProgram<sbyte>();
            public virtual short AsInt16() => ThrowInvalidProgram<short>();
            public virtual int AsInt32() => ThrowInvalidProgram<int>();
            public virtual long AsInt64() => ThrowInvalidProgram<long>();
            public virtual float AsSingle() => ThrowInvalidProgram<float>();
            public virtual double AsDouble() => ThrowInvalidProgram<double>();
            public virtual Value Clone() => ThrowInvalidProgram<Value>();
        }

        private abstract class BaseValueTypeValue : Value
        {
            public abstract int Size { get; }
        }

        // Also represents pointers and function pointer.
        private sealed class ValueTypeValue : BaseValueTypeValue, IAssignableValue
        {
            public readonly byte[] InstanceBytes;

            public override int Size => InstanceBytes.Length;

            public ValueTypeValue(TypeDesc type)
            {
                Debug.Assert(type.IsValueType || type.IsPointer || type.IsFunctionPointer);
                InstanceBytes = new byte[type.GetElementSize().AsInt];
            }

            public ValueTypeValue(byte[] bytes)
            {
                InstanceBytes = bytes;
            }

            public override Value Clone()
            {
                return new ValueTypeValue((byte[])InstanceBytes.Clone());
            }

            public override bool TryCreateByRef(out Value value)
            {
                value = new ByRefValue(InstanceBytes, 0);
                return true;
            }

            bool IAssignableValue.TryAssign(Value value)
            {
                if ((!(value is BaseValueTypeValue other) || other.Size != Size)
                    && value is not INativeIntConvertibleValue)
                {
                    ThrowHelper.ThrowInvalidProgramException();
                }

                if (!(value is ValueTypeValue vtvalue))
                {
                    return false;
                }

                Array.Copy(vtvalue.InstanceBytes, InstanceBytes, InstanceBytes.Length);
                return true;
            }

            public override bool TryCompareEquality(Value value, out bool result)
            {
                if (!(value is ValueTypeValue vtvalue)
                    || vtvalue.InstanceBytes.Length != InstanceBytes.Length)
                {
                    result = false;
                    return false;
                }

                for (int i = 0; i < InstanceBytes.Length; i++)
                {
                    if (InstanceBytes[i] != ((ValueTypeValue)value).InstanceBytes[i])
                    {
                        result = false;
                        return true;
                    }
                }

                result = true;
                return true;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                builder.EmitBytes(InstanceBytes);
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = InstanceBytes;
                return true;
            }

            private byte[] AsExactByteCount(int size)
            {
                if (InstanceBytes.Length != size)
                {
                    ThrowHelper.ThrowInvalidProgramException();
                }
                return InstanceBytes;
            }

            public override sbyte AsSByte() => (sbyte)AsExactByteCount(1)[0];
            public override short AsInt16() => BitConverter.ToInt16(AsExactByteCount(2), 0);
            public override int AsInt32() => BitConverter.ToInt32(AsExactByteCount(4), 0);
            public override long AsInt64() => BitConverter.ToInt64(AsExactByteCount(8), 0);
            public override float AsSingle() => BitConverter.ToSingle(AsExactByteCount(4), 0);
            public override double AsDouble() => BitConverter.ToDouble(AsExactByteCount(8), 0);
            public static ValueTypeValue FromSByte(sbyte value) => new ValueTypeValue(new byte[1] { (byte)value });
            public static ValueTypeValue FromInt16(short value) => new ValueTypeValue(BitConverter.GetBytes(value));
            public static ValueTypeValue FromInt32(int value) => new ValueTypeValue(BitConverter.GetBytes(value));
            public static ValueTypeValue FromInt64(long value) => new ValueTypeValue(BitConverter.GetBytes(value));
            public static ValueTypeValue FromSingle(float value) => new ValueTypeValue(BitConverter.GetBytes(value));
            public static ValueTypeValue FromDouble(double value) => new ValueTypeValue(BitConverter.GetBytes(value));
        }

        private sealed class ComInterfaceEntryArrayValue : BaseValueTypeValue
        {
            private readonly FieldDesc[] _targetFields;
            private readonly byte[][] _guidBytes;
            private readonly MetadataType _entryType;

            public override int Size => _entryType.InstanceFieldSize.AsInt * _targetFields.Length;

            public ComInterfaceEntryArrayValue(TypeDesc type, TypeDesc entryType)
            {
                Debug.Assert(IsCompatible(type, out _));
                Debug.Assert(IsComInterfaceEntryType(entryType));
                Debug.Assert(((MetadataType)type).InstanceFieldSize.AsInt % ((MetadataType)entryType).InstanceFieldSize.AsInt == 0);

                _entryType = (MetadataType)entryType;

                int numFields = ((MetadataType)type).InstanceFieldSize.AsInt / _entryType.InstanceFieldSize.AsInt;
                _targetFields = new FieldDesc[numFields];
                _guidBytes = new byte[numFields][];
                for (int i = 0; i < numFields; i++)
                    _guidBytes[i] = new byte[16];
            }

            private static bool IsComInterfaceEntryType(TypeDesc type)
                => type is MetadataType mdType
                    && mdType.Name == "ComInterfaceEntry"
                    && mdType.ContainingType is MetadataType { Name: "ComWrappers", Namespace: "System.Runtime.InteropServices" } comWrappersType
                    && comWrappersType.Module == comWrappersType.Context.SystemModule;

            public static bool IsCompatible(TypeDesc type, out TypeDesc entryType)
            {
                entryType = null;

                if (!type.IsValueType
                    || type.HasInstantiation
                    || type is not MetadataType mdType
                    || !mdType.IsSequentialLayout
                    || mdType.GetClassLayout() is not { PackingSize: 0, Size: 0 }
                    || mdType.IsInlineArray)
                {
                    return false;
                }

                foreach (FieldDesc field in type.GetFields())
                {
                    if (field.IsStatic)
                        continue;

                    entryType = field.FieldType;

                    if (!IsComInterfaceEntryType(entryType))
                        return false;
                }

                return entryType != null;
            }

            public override bool TryCompareEquality(Value value, out bool result)
            {
                result = false;
                return false;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                for (int i = 0; i < _targetFields.Length; i++)
                {
                    Debug.Assert(_entryType.GetField("IID").Offset.AsInt == 0);
                    builder.EmitBytes(_guidBytes[i]);

                    Debug.Assert(_entryType.GetField("Vtable").Offset.AsInt == _guidBytes[i].Length);
                    if (_targetFields[i] is not FieldDesc targetField)
                    {
                        builder.EmitZeroPointer();
                    }
                    else
                    {
                        Debug.Assert(targetField.IsStatic && !targetField.HasGCStaticBase && !targetField.IsThreadStatic && !targetField.HasRva);
                        ISymbolNode nonGcStaticBase = factory.TypeNonGCStaticsSymbol((MetadataType)targetField.OwningType);
                        builder.EmitPointerReloc(nonGcStaticBase, targetField.Offset.AsInt);
                    }
                }
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }

            public override bool TryCreateByRef(out Value value)
            {
                value = new ComInterfaceEntrySlotReference(this, 0);
                return true;
            }

            private sealed class ComInterfaceEntrySlotReference : ByRefValueBase, IHasInstanceFields
            {
                private readonly ComInterfaceEntryArrayValue _parent;
                private readonly int _index;

                public ComInterfaceEntrySlotReference(ComInterfaceEntryArrayValue parent, int index)
                    => (_parent, _index) = (parent, index);

                public override bool TryCompareEquality(Value value, out bool result)
                {
                    result = false;
                    return false;
                }

                public override bool GetRawData(NodeFactory factory, out object data)
                {
                    data = null;
                    return false;
                }

                public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
                {
                    throw new NotSupportedException();
                }

                bool IHasInstanceFields.TrySetField(FieldDesc field, Value value)
                {
                    if (field.OwningType != _parent._entryType)
                        return false;

                    if (field.Name == "IID"
                        && value is ValueTypeValue guidValue
                        && guidValue.Size == _parent._guidBytes[_index].Length)
                    {
                        Array.Copy(guidValue.InstanceBytes, _parent._guidBytes[_index], _parent._guidBytes[_index].Length);
                        return true;
                    }
                    else if (field.Name == "Vtable"
                        && value is ByRefValueBase byrefValue
                        && byrefValue.BackingField != null)
                    {
                        _parent._targetFields[_index] = byrefValue.BackingField;
                        return true;
                    }

                    return false;
                }

                Value IHasInstanceFields.GetField(FieldDesc field)
                {
                    // Not actually invalid, but we don't need this.
                    ThrowHelper.ThrowInvalidProgramException();
                    return null; // unreached
                }

                ByRefValueBase IHasInstanceFields.GetFieldAddress(FieldDesc field)
                {
                    if (field.OwningType == _parent._entryType)
                    {
                        // Get address of IID or Vtable field on ComInterfaceEntry this ref points to.
                        // Not actually invalid, but we don't need this.
                        ThrowHelper.ThrowInvalidProgramException();
                    }
                    else if (field.FieldType == _parent._entryType
                        && _index == 0
                        && field.Offset.AsInt % _parent._entryType.InstanceFieldSize.AsInt == 0
                        && field.Offset.AsInt < _parent._entryType.InstanceFieldSize.AsInt * _parent._targetFields.Length)
                    {
                        // Get address of a field within an array of ComInterfaceEntry.
                        int index = field.Offset.AsInt / _parent._entryType.InstanceFieldSize.AsInt;
                        return new ComInterfaceEntrySlotReference(_parent, index);
                    }

                    ThrowHelper.ThrowInvalidProgramException();
                    return null; // unreached
                }
            }
        }

        private sealed class VTableLikeStructValue : BaseValueTypeValue, IAssignableValue
        {
            private readonly MetadataType _type;
            private readonly MethodDesc[] _methods;
            private readonly FieldDesc _fieldThatOwnsMemory;

            public VTableLikeStructValue(MetadataType type, FieldDesc fieldThatOwnsMemory)
                : this(type, new MethodDesc[GetFieldCount(type)], fieldThatOwnsMemory)
            {
            }

            private VTableLikeStructValue(MetadataType type, MethodDesc[] methods, FieldDesc fieldThatOwnsMemory)
                => (_type, _methods, _fieldThatOwnsMemory) = (type, methods, fieldThatOwnsMemory);

            private static int GetFieldCount(MetadataType type)
            {
                Debug.Assert(IsCompatible(type));
                Debug.Assert(type.InstanceFieldSize.AsInt % type.Context.Target.PointerSize == 0);
                return type.InstanceFieldSize.AsInt / type.Context.Target.PointerSize;
            }

            public override int Size => _methods.Length * _type.Context.Target.PointerSize;

            public static bool IsCompatible(TypeDesc type)
            {
                if (!type.IsValueType
                    || type.HasInstantiation
                    || type is not MetadataType mdType
                    || !mdType.IsSequentialLayout
                    || mdType.GetClassLayout() is not { PackingSize: 0, Size: 0 }
                    || mdType.IsInlineArray)
                {
                    return false;
                }

                bool hasFields = false;
                foreach (FieldDesc field in type.GetFields())
                {
                    if (field.IsStatic)
                        continue;

                    hasFields = true;

                    if (field.FieldType.Category != TypeFlags.FunctionPointer)
                        return false;
                }

                return hasFields;
            }

            public override bool TryCompareEquality(Value value, out bool result)
            {
                result = false;
                return false;
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                foreach (MethodDesc method in _methods)
                {
                    if (method is null)
                        builder.EmitZeroPointer();
                    else
                        builder.EmitPointerReloc(factory.ExactCallableAddressTakenAddress(method, isUnboxingStub: false));
                }
            }

            public override bool TryCreateByRef(out Value value)
            {
                value = new VTableLikeSlotReferenceValue(this, index: 0);
                return true;
            }

            public override Value Clone()
            {
                return new VTableLikeStructValue(_type, (MethodDesc[])_methods.Clone(), fieldThatOwnsMemory: null);
            }

            bool IAssignableValue.TryAssign(Value value)
            {
                if (value is not VTableLikeStructValue other)
                    return false;

                if (other.Size > Size)
                    return false;

                Array.Copy(other._methods, _methods, other._methods.Length);
                return true;
            }

            private sealed class VTableLikeSlotReferenceValue : ByRefValueBase, IHasInstanceFields
            {
                private readonly VTableLikeStructValue _parent;
                private readonly int _index;

                public override FieldDesc BackingField => _index == 0 ? _parent._fieldThatOwnsMemory : null;

                public VTableLikeSlotReferenceValue(VTableLikeStructValue parent, int index)
                    => (_parent, _index) = (parent, index);

                public override bool TryCompareEquality(Value value, out bool result)
                {
                    result = false;
                    return false;
                }

                public override bool GetRawData(NodeFactory factory, out object data)
                {
                    data = null;
                    return false;
                }

                public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
                {
                    throw new NotSupportedException();
                }

                public override bool TryStore(Value value)
                {
                    if (value is MethodPointerValue methodPointer)
                    {
                        _parent._methods[_index] = methodPointer.PointedToMethod;
                        return true;
                    }
                    else if (value is VTableLikeStructValue otherStruct
                        && _parent._methods.Length - _index >= otherStruct._methods.Length)
                    {
                        Array.Copy(otherStruct._methods, 0, _parent._methods, _index, otherStruct._methods.Length);
                        return true;
                    }

                    return false;
                }

                public override bool TryLoad(TypeDesc type, out Value value)
                {
                    if (!VTableLikeStructValue.IsCompatible(type)
                        || type is not MetadataType mdType
                        || mdType.InstanceFieldSize.AsInt > (_parent._methods.Length - _index) * _parent._type.Context.Target.PointerSize)
                    {
                        value = null;
                        return false;
                    }

                    MethodDesc[] slots = new MethodDesc[GetFieldCount(mdType)];
                    Array.Copy(_parent._methods, _index, slots, 0, slots.Length);
                    value = new VTableLikeStructValue(mdType, slots, fieldThatOwnsMemory: null);
                    return true;
                }

                public override Value Clone() => this; // The reference is immutable

                private int GetFieldIndex(FieldDesc field)
                {
                    // Not actually invalid program, just difficult to model
                    if (!VTableLikeStructValue.IsCompatible(field.OwningType))
                        ThrowHelper.ThrowInvalidProgramException();

                    Debug.Assert(field.Offset.AsInt % _parent._type.Context.Target.PointerSize == 0 && field.FieldType.IsFunctionPointer);

                    int index = (field.Offset.AsInt / _parent._type.Context.Target.PointerSize) + _index;
                    if (index >= _parent._methods.Length)
                        ThrowHelper.ThrowInvalidProgramException();

                    return index;
                }

                bool IHasInstanceFields.TrySetField(FieldDesc field, Value value)
                {
                    if (value is not MethodPointerValue methodPtr)
                        return false;

                    _parent._methods[GetFieldIndex(field)] = methodPtr.PointedToMethod;
                    return true;
                }

                Value IHasInstanceFields.GetField(FieldDesc field)
                {
                    MethodDesc method = _parent._methods[GetFieldIndex(field)];

                    if (method is not null)
                        return new MethodPointerValue(method);
                    else
                        return _parent._type.Context.Target.PointerSize == 8 ? ValueTypeValue.FromInt64(0) : ValueTypeValue.FromInt32(0);
                }

                ByRefValueBase IHasInstanceFields.GetFieldAddress(FieldDesc field)
                {
                    return new VTableLikeSlotReferenceValue(_parent, GetFieldIndex(field));
                }

                public override bool TryInitialize(int size)
                {
                    if (size % _parent._type.Context.Target.PointerSize != 0)
                        return false;

                    int numSlots = size / _parent._type.Context.Target.PointerSize;
                    if (_index + numSlots > _parent._methods.Length)
                        return false;

                    for (int i = _index; i < numSlots; i++)
                        _parent._methods[i] = null;

                    return true;
                }
            }
        }

        private sealed class RuntimeFieldHandleValue : BaseValueTypeValue, IInternalModelingOnlyValue
        {
            public FieldDesc Field { get; private set; }

            public RuntimeFieldHandleValue(FieldDesc field)
            {
                Field = field;
            }

            public override int Size => Field.Context.Target.PointerSize;

            public override bool TryCompareEquality(Value value, out bool result)
            {
                if (!(value is RuntimeFieldHandleValue))
                {
                    result = false;
                    return false;
                }

                result = Field == ((RuntimeFieldHandleValue)value).Field;
                return true;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                throw new NotSupportedException();
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }
        }

        private sealed class RuntimeTypeHandleValue : BaseValueTypeValue, IInternalModelingOnlyValue
        {
            public TypeDesc Type { get; }

            public RuntimeTypeHandleValue(TypeDesc type)
            {
                Type = type;
            }

            public override int Size => Type.Context.Target.PointerSize;

            public override bool TryCompareEquality(Value value, out bool result)
            {
                if (!(value is RuntimeTypeHandleValue))
                {
                    result = false;
                    return false;
                }

                result = Type == ((RuntimeTypeHandleValue)value).Type;
                return true;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                throw new NotSupportedException();
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }
        }

        private sealed class RuntimeTypeValue : ReferenceTypeValue
        {
            public TypeDesc TypeRepresented { get; }

            public RuntimeTypeValue(TypeDesc type)
                : base(type.Context.SystemModule.GetKnownType("System", "RuntimeType"))
            {
                TypeRepresented = type;
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = factory.SerializedMaximallyConstructableRuntimeTypeObject(TypeRepresented);
                return true;
            }

            public override ReferenceTypeValue ToForeignInstance(int baseInstructionCounter, TypePreinit preinitContext)
            {
                if (!preinitContext._internedTypes.TryGetValue(TypeRepresented, out RuntimeTypeValue result))
                {
                    preinitContext._internedTypes.Add(TypeRepresented, result = new RuntimeTypeValue(TypeRepresented));
                }
                return result;
            }
            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                builder.EmitPointerReloc(factory.SerializedMaximallyConstructableRuntimeTypeObject(TypeRepresented));
            }
        }

        private sealed class SpanValue : BaseValueTypeValue, IInternalModelingOnlyValue
        {
            private readonly MetadataType _elementType;
            private byte[] _bytes;
            private int _index;
            private int _length;

            public SpanValue(MetadataType elementType, byte[] bytes, int index, int length)
            {
                Debug.Assert(index <= bytes.Length);
                Debug.Assert(length <= bytes.Length - index);
                _elementType = elementType;
                _bytes = bytes;
                _index = index;
                _length = length;
            }

            public override int Size => 2 * _elementType.Context.Target.PointerSize;

            public override bool TryCompareEquality(Value value, out bool result)
            {
                result = false;
                return false;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                throw new NotSupportedException();
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }

            public override Value Clone()
            {
                return new SpanValue(_elementType, _bytes, _index, _length);
            }

            public override bool TryCreateByRef(out Value value)
            {
                value = new SpanReferenceValue(this);
                return true;
            }

            private sealed class SpanReferenceValue : ByRefValueBase, IHasInstanceFields
            {
                private readonly SpanValue _value;

                public SpanReferenceValue(SpanValue value)
                {
                    _value = value;
                }

                public override bool TryCompareEquality(Value value, out bool result)
                {
                    result = false;
                    return false;
                }

                public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
                {
                    throw new NotSupportedException();
                }

                public override bool GetRawData(NodeFactory factory, out object data)
                {
                    data = null;
                    return false;
                }

                public bool TrySetField(FieldDesc field, Value value)
                {
                    MetadataType elementType;
                    if (!TryGetSpanElementType(field.OwningType, isReadOnlySpan: true, out elementType)
                        && !TryGetSpanElementType(field.OwningType, isReadOnlySpan: false, out elementType))
                        return false;

                    if (elementType != _value._elementType)
                        return false;

                    if (field.Name == "_length")
                    {
                        _value._length = value.AsInt32() * _value._elementType.InstanceFieldSize.AsInt;
                        return true;
                    }

                    if (value is ByRefValue byref)
                    {
                        Debug.Assert(field.Name == "_reference");
                        _value._bytes = byref.PointedToBytes;
                        _value._index = byref.PointedToOffset;
                        return true;
                    }

                    return false;
                }

                public Value GetField(FieldDesc field)
                {
                    MetadataType elementType;
                    if (!TryGetSpanElementType(field.OwningType, isReadOnlySpan: true, out elementType)
                        && !TryGetSpanElementType(field.OwningType, isReadOnlySpan: false, out elementType))
                        ThrowHelper.ThrowInvalidProgramException();

                    if (elementType != _value._elementType)
                        ThrowHelper.ThrowInvalidProgramException();

                    if (field.Name == "_length")
                        return ValueTypeValue.FromInt32(_value._length / elementType.InstanceFieldSize.AsInt);

                    Debug.Assert(field.Name == "_reference");
                    return new ByRefValue(_value._bytes, _value._index);
                }

                public ByRefValueBase GetFieldAddress(FieldDesc field)
                {
                    ThrowHelper.ThrowInvalidProgramException();
                    return null; // unreached
                }
            }
        }

        private sealed class MethodPointerValue : BaseValueTypeValue, IInternalModelingOnlyValue
        {
            public MethodDesc PointedToMethod { get; }

            public MethodPointerValue(MethodDesc pointedToMethod)
            {
                PointedToMethod = pointedToMethod;
            }

            public override int Size => PointedToMethod.Context.Target.PointerSize;

            public override bool TryCompareEquality(Value value, out bool result)
            {
                if (!(value is MethodPointerValue))
                {
                    result = false;
                    return false;
                }

                result = PointedToMethod == ((MethodPointerValue)value).PointedToMethod;
                return true;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                throw new NotSupportedException();
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }

            public override Value Clone() => this; // Immutable
        }

        private abstract class ByRefValueBase : Value, INativeIntConvertibleValue
        {
            public virtual bool TryStore(Value value) => false;
            public virtual bool TryLoad(TypeDesc type, out Value value)
            {
                value = null;
                return false;
            }
            public virtual bool TryInitialize(int size) => false;

            public virtual FieldDesc BackingField => null;
        }

        private sealed class ByRefValue : ByRefValueBase, IHasInstanceFields
        {
            public readonly byte[] PointedToBytes;
            public readonly int PointedToOffset;

            public ByRefValue(byte[] pointedToBytes, int pointedToOffset)
            {
                PointedToBytes = pointedToBytes;
                PointedToOffset = pointedToOffset;
            }

            public override bool TryCompareEquality(Value value, out bool result)
            {
                if (!(value is ByRefValue))
                {
                    result = false;
                    return false;
                }

                result = PointedToBytes == ((ByRefValue)value).PointedToBytes
                    && PointedToOffset == ((ByRefValue)value).PointedToOffset;
                return true;
            }

            Value IHasInstanceFields.GetField(FieldDesc field) => new FieldAccessor(PointedToBytes, PointedToOffset).GetField(field);
            bool IHasInstanceFields.TrySetField(FieldDesc field, Value value) => new FieldAccessor(PointedToBytes, PointedToOffset).TrySetField(field, value);
            ByRefValueBase IHasInstanceFields.GetFieldAddress(FieldDesc field) => new FieldAccessor(PointedToBytes, PointedToOffset).GetFieldAddress(field);

            public override bool TryInitialize(int size)
            {
                if ((uint)size > (uint)(PointedToBytes.Length - PointedToOffset))
                {
                    return false;
                }

                for (int i = PointedToOffset; i < PointedToOffset + size; i++)
                {
                    PointedToBytes[i] = 0;
                }

                return true;
            }

            public override bool TryStore(Value value)
            {
                if (value is not ValueTypeValue srcVal)
                    return false;

                byte[] src = srcVal.InstanceBytes;
                if (PointedToOffset + src.Length > PointedToBytes.Length)
                    return false;

                Array.Copy(src, 0, PointedToBytes, PointedToOffset, src.Length);

                return true;
            }

            public override bool TryLoad(TypeDesc type, out Value value)
            {
                if (!type.IsValueType
                    || ((MetadataType)type).InstanceFieldSize.AsInt > PointedToBytes.Length - PointedToOffset)
                {
                    value = null;
                    return false;
                }

                var result = new ValueTypeValue(type);
                Array.Copy(PointedToBytes, PointedToOffset, result.InstanceBytes, 0, result.InstanceBytes.Length);
                value = result;
                return true;
            }

            public override Value Clone() => this; // Immutable

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                // This would imply we have a byref-typed static field. The layout algorithm should have blocked this.
                throw new NotImplementedException();
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = null;
                return false;
            }
        }

        private abstract class ReferenceTypeValue : Value
        {
            public TypeDesc Type { get; }

            protected ReferenceTypeValue(TypeDesc type) { Type = type; }

            public override bool TryCompareEquality(Value value, out bool result)
            {
                result = this == value;
                return true;
            }

            public abstract ReferenceTypeValue ToForeignInstance(int baseInstructionCounter, TypePreinit preinitContext);
        }

        private struct AllocationSite
        {
            public MetadataType OwningType { get; }
            public int InstructionCounter { get; }
            public AllocationSite(MetadataType type, int instructionCounter)
            {
                Debug.Assert(type.HasStaticConstructor);
                OwningType = type;
                InstructionCounter = instructionCounter;
            }
        }

        /// <summary>
        /// A reference type that is not a string literal.
        /// </summary>
        private abstract class AllocatedReferenceTypeValue : ReferenceTypeValue
        {
            protected AllocationSite AllocationSite { get; }

            public AllocatedReferenceTypeValue(TypeDesc type, AllocationSite allocationSite)
                : base(type)
            {
                AllocationSite = allocationSite;
            }

            public override ReferenceTypeValue ToForeignInstance(int baseInstructionCounter, TypePreinit preinitContext) =>
                new ForeignTypeInstance(
                    Type,
                    new AllocationSite(AllocationSite.OwningType, AllocationSite.InstructionCounter - baseInstructionCounter),
                    this);

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                if (this is ISerializableReference serializableRef)
                {
                    data = factory.SerializedFrozenObject(AllocationSite.OwningType, AllocationSite.InstructionCounter, serializableRef);
                    return true;
                }
                data = null;
                return false;
            }

            public virtual bool HasConditionalDependencies => false;

            public virtual void GetConditionalDependencies(ref CombinedDependencyList dependencies, NodeFactory factory)
            {
            }
        }

        private sealed class DelegateInstance : AllocatedReferenceTypeValue, ISerializableReference
        {
            private readonly MethodDesc _methodPointed;
            private readonly ReferenceTypeValue _firstParameter;

            public DelegateInstance(TypeDesc delegateType, MethodDesc methodPointed, ReferenceTypeValue firstParameter, AllocationSite allocationSite)
                : base(delegateType, allocationSite)
            {
                _methodPointed = methodPointed;
                _firstParameter = firstParameter;
            }

            private DelegateCreationInfo GetDelegateCreationInfo(NodeFactory factory)
                => DelegateCreationInfo.Create(
                    Type.ConvertToCanonForm(CanonicalFormKind.Specific),
                    _methodPointed,
                    constrainedType: null,
                    factory,
                    followVirtualDispatch: false);

            public override bool HasConditionalDependencies => true;

            public override void GetConditionalDependencies(ref CombinedDependencyList dependencies, NodeFactory factory)
            {
                dependencies ??= new CombinedDependencyList();

                DelegateCreationInfo creationInfo = GetDelegateCreationInfo(factory);

                MethodDesc targetMethod = creationInfo.PossiblyUnresolvedTargetMethod.GetCanonMethodTarget(CanonicalFormKind.Specific);
                factory.MetadataManager.GetDependenciesDueToDelegateCreation(ref dependencies, factory, creationInfo.DelegateType, targetMethod);
            }

            public void WriteContent(ref ObjectDataBuilder builder, ISymbolNode thisNode, NodeFactory factory)
            {
                Debug.Assert(_methodPointed.Signature.IsStatic == (_firstParameter == null));

                DelegateCreationInfo creationInfo = GetDelegateCreationInfo(factory);

                Debug.Assert(!creationInfo.TargetNeedsVTableLookup);

                // MethodTable
                var node = factory.ConstructedTypeSymbol(Type);
                Debug.Assert(!node.RepresentsIndirectionCell);  // Shouldn't have allowed this
                builder.EmitPointerReloc(node);

                if (_methodPointed.Signature.IsStatic)
                {
                    Debug.Assert(creationInfo.Constructor.Method.Name == "InitializeOpenStaticThunk");

                    // _firstParameter
                    builder.EmitPointerReloc(thisNode);

                    // _helperObject
                    builder.EmitZeroPointer();

                    // _extraFunctionPointerOrData
                    builder.EmitPointerReloc(creationInfo.GetTargetNode(factory));

                    // _functionPointer
                    Debug.Assert(creationInfo.Thunk != null);
                    builder.EmitPointerReloc(creationInfo.Thunk);
                }
                else
                {
                    Debug.Assert(creationInfo.Constructor.Method.Name == "InitializeClosedInstance");

                    // _firstParameter
                    _firstParameter.WriteFieldData(ref builder, factory);

                    // _helperObject
                    builder.EmitZeroPointer();

                    // _extraFunctionPointerOrData
                    builder.EmitZeroPointer();

                    // _functionPointer
                    builder.EmitPointerReloc(creationInfo.GetTargetNode(factory));
                }
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                builder.EmitPointerReloc(factory.SerializedFrozenObject(AllocationSite.OwningType, AllocationSite.InstructionCounter, this));
            }

            public bool IsKnownImmutable => _methodPointed.Signature.IsStatic;

            public int ArrayLength => throw new NotSupportedException();
        }

        private sealed class ArrayInstance : AllocatedReferenceTypeValue, ISerializableReference
        {
            private readonly int _elementCount;
            private readonly int _elementSize;
            private readonly byte[] _data;

            public ArrayInstance(ArrayType type, int elementCount, AllocationSite allocationSite)
                : base(type, allocationSite)
            {
                _elementCount = elementCount;
                _elementSize = type.ElementType.GetElementSize().AsInt;
                _data = new byte[elementCount * _elementSize];
            }

            public bool TryInitialize(byte[] bytes)
            {
                if (bytes.Length != _data.Length)
                    return false;

                Array.Copy(bytes, _data, bytes.Length);
                return true;
            }

            public int Length
            {
                get
                {
                    return _elementCount;
                }
            }

            public bool TryStoreElement(int index, Value value)
            {
                if (!(value is ValueTypeValue valueToStore))
                    return false;

                if ((uint)index >= (uint)Length)
                    return false;

                Debug.Assert(valueToStore.InstanceBytes.Length == _elementSize);
                Array.Copy(valueToStore.InstanceBytes, 0, _data, index * _elementSize, valueToStore.InstanceBytes.Length);
                return true;
            }

            public bool TryLoadElement(int index, out Value value)
            {
                if ((uint)index > (uint)Length)
                {
                    value = null;
                    return false;
                }

                ValueTypeValue result = new ValueTypeValue(((ArrayType)Type).ElementType);
                Array.Copy(_data, index * _elementSize, result.InstanceBytes, 0, _elementSize);
                value = result;
                return true;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                builder.EmitPointerReloc(factory.SerializedFrozenObject(AllocationSite.OwningType, AllocationSite.InstructionCounter, this));
            }

            public void WriteContent(ref ObjectDataBuilder builder, ISymbolNode thisNode, NodeFactory factory)
            {
                // MethodTable
                var node = factory.ConstructedTypeSymbol(Type);
                Debug.Assert(!node.RepresentsIndirectionCell);  // Arrays are always local
                builder.EmitPointerReloc(node);

                // numComponents
                builder.EmitInt(_elementCount);

                int pointerSize = Type.Context.Target.PointerSize;
                Debug.Assert(pointerSize == 8 || pointerSize == 4);

                if (pointerSize == 8)
                {
                    // padding numComponents in 64-bit
                    builder.EmitInt(0);
                }

                builder.EmitBytes(_data);
            }

            public ByRefValue GetArrayData()
            {
                return new ByRefValue(_data, 0);
            }

            public bool IsKnownImmutable => _elementCount == 0;

            public int ArrayLength => Length;
        }

        private sealed class ForeignTypeInstance : AllocatedReferenceTypeValue
        {
            public ReferenceTypeValue Data { get; }

            public ForeignTypeInstance(TypeDesc type, AllocationSite allocationSite, ReferenceTypeValue data)
                : base(type, allocationSite)
            {
                Data = data;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                if (Data is ISerializableReference serializableReference)
                {
                    builder.EmitPointerReloc(factory.SerializedFrozenObject(AllocationSite.OwningType, AllocationSite.InstructionCounter, serializableReference));
                }
                else
                {
                    Data.WriteFieldData(ref builder, factory);
                }
            }

            public override ReferenceTypeValue ToForeignInstance(int baseInstructionCounter, TypePreinit preinitContext) => this;
        }

        private sealed class StringInstance : ReferenceTypeValue, IHasInstanceFields
        {
            private readonly byte[] _value;

            private string ValueAsString
            {
                get
                {
                    FieldDesc firstCharField = Type.GetField("_firstChar");
                    int startOffset = firstCharField.Offset.AsInt;
                    int length = _value.Length - startOffset - sizeof(char) /* terminating null */;
                    return new string(MemoryMarshal.Cast<byte, char>(
                        ((ReadOnlySpan<byte>)_value).Slice(startOffset, length)));
                }
            }
            public StringInstance(TypeDesc stringType, string value)
                : base(stringType)
            {
                _value = ConstructStringInstance(stringType, value);
            }

            private static byte[] ConstructStringInstance(TypeDesc stringType, ReadOnlySpan<char> value)
            {
                int pointerSize = stringType.Context.Target.PointerSize;
                var bytes = new byte[
                    pointerSize /* MethodTable */
                    + sizeof(int) /* length */
                    + (value.Length * sizeof(char)) /* bytes */
                    + sizeof(char) /* null terminator */];

                FieldDesc lengthField = stringType.GetField("_stringLength");
                Debug.Assert(lengthField.FieldType.IsWellKnownType(WellKnownType.Int32)
                    && lengthField.Offset.AsInt == pointerSize);
                bool success = new FieldAccessor(bytes).TrySetField(lengthField, ValueTypeValue.FromInt32(value.Length));
                Debug.Assert(success);

                FieldDesc firstCharField = stringType.GetField("_firstChar");
                Debug.Assert(firstCharField.FieldType.IsWellKnownType(WellKnownType.Char)
                    && firstCharField.Offset.AsInt == pointerSize + sizeof(int) /* length */);

                value.CopyTo(MemoryMarshal.Cast<byte, char>(((Span<byte>)bytes).Slice(firstCharField.Offset.AsInt)));

                return bytes;
            }

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                builder.EmitPointerReloc(factory.SerializedStringObject(ValueAsString));
            }

            public override bool GetRawData(NodeFactory factory, out object data)
            {
                data = factory.SerializedStringObject(ValueAsString);
                return true;
            }

            public override ReferenceTypeValue ToForeignInstance(int baseInstructionCounter, TypePreinit preinitContext)
            {
                string value = ValueAsString;
                if (!preinitContext._internedStrings.TryGetValue(value, out StringInstance result))
                {
                    preinitContext._internedStrings.Add(value, result = new StringInstance(Type, value));
                }
                return result;
            }
            Value IHasInstanceFields.GetField(FieldDesc field) => new FieldAccessor(_value).GetField(field);
            bool IHasInstanceFields.TrySetField(FieldDesc field, Value value) => false;
            ByRefValueBase IHasInstanceFields.GetFieldAddress(FieldDesc field) => new FieldAccessor(_value).GetFieldAddress(field);
        }

        private sealed class ObjectInstance : AllocatedReferenceTypeValue, IHasInstanceFields, ISerializableReference
        {
            private readonly byte[] _data;

            public ObjectInstance(DefType type, AllocationSite allocationSite)
                : base(type, allocationSite)
            {
                int size = type.InstanceByteCount.AsInt;
                if (type.IsValueType)
                    size += type.Context.Target.PointerSize;
                _data = new byte[size];
            }

            public static bool TryBox(DefType type, Value value, AllocationSite allocationSite, out ObjectInstance result)
            {
                if (!(value is BaseValueTypeValue))
                    ThrowHelper.ThrowInvalidProgramException();

                if (!(value is ValueTypeValue valuetype))
                {
                    result = null;
                    return false;
                }

                result = new ObjectInstance(type, allocationSite);
                Array.Copy(valuetype.InstanceBytes, 0, result._data, type.Context.Target.PointerSize, valuetype.InstanceBytes.Length);
                return true;
            }

            public bool TryUnboxAny(TypeDesc type, out Value value)
            {
                value = null;

                if (!type.IsValueType || type.IsNullable)
                    return false;

                if (Type.UnderlyingType != type.UnderlyingType)
                    return false;

                var result = new ValueTypeValue(type);
                Array.Copy(_data, type.Context.Target.PointerSize, result.InstanceBytes, 0, result.InstanceBytes.Length);
                value = result;
                return true;
            }

            Value IHasInstanceFields.GetField(FieldDesc field) => new FieldAccessor(_data).GetField(field);
            bool IHasInstanceFields.TrySetField(FieldDesc field, Value value) => new FieldAccessor(_data).TrySetField(field, value);
            ByRefValueBase IHasInstanceFields.GetFieldAddress(FieldDesc field) => new FieldAccessor(_data).GetFieldAddress(field);

            public override void WriteFieldData(ref ObjectDataBuilder builder, NodeFactory factory)
            {
                builder.EmitPointerReloc(factory.SerializedFrozenObject(AllocationSite.OwningType, AllocationSite.InstructionCounter, this));
            }

            public void WriteContent(ref ObjectDataBuilder builder, ISymbolNode thisNode, NodeFactory factory)
            {
                // MethodTable
                var node = factory.ConstructedTypeSymbol(Type);
                Debug.Assert(!node.RepresentsIndirectionCell);  // Shouldn't have allowed preinitializing this
                builder.EmitPointerReloc(node);

                // We skip the first pointer because that's the MethodTable pointer
                // we just initialized above.
                int pointerSize = factory.Target.PointerSize;
                builder.EmitBytes(_data, pointerSize, _data.Length - pointerSize);
            }

            public bool IsKnownImmutable => !Type.GetFields().GetEnumerator().MoveNext();

            public int ArrayLength => throw new NotSupportedException();
        }

        private struct FieldAccessor
        {
            private readonly byte[] _instanceBytes;
            private readonly int _offset;

            public FieldAccessor(byte[] bytes, int offset = 0)
            {
                _instanceBytes = bytes;
                _offset = offset;
            }

            public ValueTypeValue GetField(FieldDesc field)
            {
                Debug.Assert(!field.IsStatic);
                Debug.Assert(!field.FieldType.IsGCPointer);
                int fieldOffset = field.Offset.AsInt;
                int fieldSize = field.FieldType.GetElementSize().AsInt;
                if (fieldOffset + fieldSize > _instanceBytes.Length - _offset)
                    ThrowHelper.ThrowInvalidProgramException();

                var result = new ValueTypeValue(field.FieldType);
                Array.Copy(_instanceBytes, _offset + fieldOffset, result.InstanceBytes, 0, fieldSize);
                return result;
            }

            public bool TrySetField(FieldDesc field, Value value)
            {
                Debug.Assert(!field.IsStatic);

                if (field.FieldType.IsGCPointer)
                {
                    // Allow setting reference type fields to null. Since this is the only value we can
                    // write, this is a no-op since reference type fields are always null
                    Debug.Assert(value == null);
                    return true;
                }

                int fieldOffset = field.Offset.AsInt;
                int fieldSize = field.FieldType.GetElementSize().AsInt;
                if (fieldOffset + fieldSize > _instanceBytes.Length - _offset)
                    ThrowHelper.ThrowInvalidProgramException();

                if (value is IInternalModelingOnlyValue)
                {
                    return false;
                }

                if (value is ByRefValueBase
                    && (field.FieldType.IsWellKnownType(WellKnownType.IntPtr) || field.FieldType.IsWellKnownType(WellKnownType.UIntPtr)))
                {
                    return false;
                }

                if (value is not ValueTypeValue vtValue)
                {
                    ThrowHelper.ThrowInvalidProgramException();
                    return false; // unreached
                }

                Array.Copy(vtValue.InstanceBytes, 0, _instanceBytes, _offset + fieldOffset, fieldSize);
                return true;
            }

            public ByRefValue GetFieldAddress(FieldDesc field)
            {
                Debug.Assert(!field.IsStatic);
                Debug.Assert(!field.FieldType.IsGCPointer);
                int fieldOffset = field.Offset.AsInt;
                int fieldSize = field.FieldType.GetElementSize().AsInt;
                if (fieldOffset + fieldSize > _instanceBytes.Length - _offset)
                    ThrowHelper.ThrowInvalidProgramException();

                return new ByRefValue(_instanceBytes, _offset + fieldOffset);
            }
        }

        private struct StackEntry
        {
            public readonly StackValueKind ValueKind;
            public readonly Value Value;

            public StackEntry(StackValueKind valueKind, Value value)
            {
                // TODO: can we assert invariants around value allowed for valueKind?
                ValueKind = valueKind;
                Value = value;
            }
        }

        private struct Status
        {
            public string FailureReason { get; }

            public static Status Success => default;

            public bool IsSuccessful => FailureReason == null;

            private Status(string message)
            {
                FailureReason = message;
            }

            public static Status Fail(MethodDesc method, ILOpcode opcode, string detail = null)
            {
                return new Status($"Method '{method}', opcode '{opcode}' {detail ?? ""}");
            }

            public static Status Fail(MethodDesc method, string detail)
            {
                return new Status($"Method '{method}': {detail}");
            }
        }

        private readonly struct NestedPreinitResult
        {
            private readonly Dictionary<FieldDesc, Value> _fieldValues;
            private readonly int _baseInstructionCounter;

            public NestedPreinitResult(Dictionary<FieldDesc, Value> fieldValues, int baseInstructionCounter)
                => (_fieldValues, _baseInstructionCounter) = (fieldValues, baseInstructionCounter);

            public bool TryGetFieldValue(TypePreinit context, FieldDesc field, out Value value)
            {
                Value fieldValue = _fieldValues[field];

                if (fieldValue is ReferenceTypeValue referenceType)
                {
                    value = referenceType.ToForeignInstance(_baseInstructionCounter, context);
                    return true;
                }
                else if (fieldValue is BaseValueTypeValue)
                {
                    value = fieldValue;
                    return true;
                }

                value = null;
                return false;
            }
        }

        public class PreinitializationInfo
        {
            private readonly Dictionary<FieldDesc, ISerializableValue> _fieldValues;

            public MetadataType Type { get; }

            public string FailureReason { get; }

            public bool IsPreinitialized => _fieldValues != null;

            public PreinitializationInfo(MetadataType type, IEnumerable<KeyValuePair<FieldDesc, ISerializableValue>> fieldValues)
            {
                Type = type;
                _fieldValues = new Dictionary<FieldDesc, ISerializableValue>();
                foreach (var field in fieldValues)
                    _fieldValues.Add(field.Key, field.Value);
            }

            public PreinitializationInfo(MetadataType type, string failureReason)
            {
                Type = type;
                FailureReason = failureReason;
            }

            public ISerializableValue GetFieldValue(FieldDesc field)
            {
                Debug.Assert(IsPreinitialized);
                Debug.Assert(field.OwningType == Type);
                Debug.Assert(field.IsStatic && !field.HasRva && !field.IsThreadStatic && !field.IsLiteral);
                return _fieldValues[field];
            }
        }

        public abstract class TypePreinitializationPolicy
        {
            /// <summary>
            /// Returns true if the preinitialization system may attempt to preinitialize this type.
            /// </summary>
            public abstract bool CanPreinitialize(DefType type);

            /// <summary>
            /// Returns true if all concrete forms of this canonical form will be preinitialized.
            /// This can only be answered by a whole program view.
            /// </summary>
            public abstract bool CanPreinitializeAllConcreteFormsForCanonForm(DefType type);
        }

        /// <summary>
        /// Preinitialization policy that doesn't allow preinitialization.
        /// </summary>
        public sealed class DisabledPreinitializationPolicy : TypePreinitializationPolicy
        {
            public override bool CanPreinitialize(DefType type) => false;
            public override bool CanPreinitializeAllConcreteFormsForCanonForm(DefType type) => false;
        }

        /// <summary>
        /// Preinitialization policy that assumes new canonical forms of types could be created
        /// at runtime.
        /// </summary>
        public sealed class TypeLoaderAwarePreinitializationPolicy : TypePreinitializationPolicy
        {
            public override bool CanPreinitialize(DefType type) => true;

            public override bool CanPreinitializeAllConcreteFormsForCanonForm(DefType type) => false;
        }
    }

#pragma warning disable SA1400 // Element 'Extensions' should declare an access modifier
    file static class Extensions
    {
        public static StackValueKind WithNormalizedNativeInt(this StackValueKind kind, TypeSystemContext context)
            => kind switch
            {
                StackValueKind.NativeInt => context.Target.PointerSize == 8 ? StackValueKind.Int64 : StackValueKind.Int32,
                _ => kind
            };
    }
}
