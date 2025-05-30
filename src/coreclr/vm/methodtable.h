// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// File: methodtable.h
//

#ifndef _METHODTABLE_H_
#define _METHODTABLE_H_

/*
 *  Include Files
 */
#include "vars.hpp"
#include "cor.h"
#include "hash.h"
#include "crst.h"
#include "cgensys.h"
#ifdef FEATURE_COMINTEROP
#include "stdinterfaces.h"
#endif
#include "slist.h"
#include "spinlock.h"
#include "typehandle.h"
#include "eehash.h"
#include "contractimpl.h"
#include "generics.h"
#include "gcinfotypes.h"
#include "enum_class_flags.h"
#include "threadstatics.h"

/*
 * Forward Declarations
 */
class    AppDomain;
class    ArrayClass;
class    ArrayMethodDesc;
class ClassLoader;
class FCallMethodDesc;
class    EEClass;
class    EnCFieldDesc;
class FieldDesc;
class JIT_TrialAlloc;
class MetaSig;
class    MethodDesc;
class    MethodDescChunk;
class    MethodTable;
class    Module;
class    Object;
class    Stub;
class    Substitution;
class    TypeHandle;
class   Dictionary;
class   AllocMemTracker;
class   SimpleRWLock;
class   MethodDataCache;
class   EEClassLayoutInfo;
class   EEClassNativeLayoutInfo;
#ifdef FEATURE_COMINTEROP
class   ComCallWrapperTemplate;
#endif
#ifdef FEATURE_COMINTEROP_UNMANAGED_ACTIVATION
class ClassFactoryBase;
#endif // FEATURE_COMINTEROP_UNMANAGED_ACTIVATION
class ArgDestination;
enum class WellKnownAttribute : DWORD;
enum class AsyncVariantLookup;
struct MethodTableAuxiliaryData;

typedef DPTR(MethodTableAuxiliaryData) PTR_MethodTableAuxiliaryData;
typedef DPTR(MethodTableAuxiliaryData const) PTR_Const_MethodTableAuxiliaryData;

enum class StaticsOffsetType
{
    Normal,
    ThreadLocal
};

enum class ResolveVirtualStaticMethodFlags
{
    None = 0,
    AllowNullResult = 1,
    VerifyImplemented = 2,
    AllowVariantMatches = 4,
    InstantiateResultOverFinalMethodDesc = 8,

    support_use_as_flags // Enable the template functions in enum_class_flags.h
};


enum class FindDefaultInterfaceImplementationFlags
{
    None,
    AllowVariance = 1,
    ThrowOnConflict = 2,
    InstantiateFoundMethodDesc = 4,

    support_use_as_flags // Enable the template functions in enum_class_flags.h
};

enum class MethodTableStaticsFlags
{
    None = 0,
    Present = 0x1,
    Generic = 0x2,
    Thread = 0x4,

    support_use_as_flags // Enable the template functions in enum_class_flags.h
};

enum class MethodDataComputeOptions
{
    NoCache, // Do not place the results of getting the MethodData into the cache, but use it if it is there
    NoCacheVirtualsOnly, // Do not place the results of getting the MethodData into the cache, but use it if it is there. If freshly computed, only fill in virtual data, and ignore non-virtuals
    Cache, // Place result of getting MethodData into the cache if it is not already there
    CacheOnly, // Get the MethodData from the cache. If not present, simply do not return one
};

//============================================================================
// This is the in-memory structure of a class and it will evolve.
//============================================================================

// <TODO>
// Add a sync block
// Also this class currently has everything public - this may changes
// Might also need to hold onto the meta data loader fot this class</TODO>

//
// A MethodTable contains an array of these structures, which describes each interface implemented
// by this class (directly declared or indirectly declared).
//
// Generic type instantiations (in C# syntax: C<ty_1,...,ty_n>) are represented by
// MethodTables, i.e. a new MethodTable gets allocated for each such instantiation.
// The entries in these tables (i.e. the code) are, however, often shared.
//
// In particular, a MethodTable's vtable contents (and hence method descriptors) may be
// shared between compatible instantiations (e.g. List<string> and List<object> have
// the same vtable *contents*).  Likewise the EEClass will be shared between
// compatible instantiations whenever the vtable contents are.
//
// !!! Thus that it is _not_ generally the case that GetClass.GetMethodTable() == t. !!!
//
// Instantiated interfaces have their own method tables unique to the instantiation e.g. I<string> is
// distinct from I<int> and I<object>
//
// For generic types the interface map lists generic interfaces
// For instantiated types the interface map lists instantiated interfaces
//   e.g. for C<T> : I<T>, J<string>
// the interface map for C would list I and J
// the interface map for C<int> would list I<int> and J<string>
//
struct InterfaceInfo_t
{

    // Method table of the interface
    PTR_MethodTable m_pMethodTable;

public:
    FORCEINLINE PTR_MethodTable GetMethodTable()
    {
        LIMITED_METHOD_CONTRACT;
        return VolatileLoadWithoutBarrier(&m_pMethodTable);
    }

#ifndef DACCESS_COMPILE
    void SetMethodTable(MethodTable * pMT)
    {
        LIMITED_METHOD_CONTRACT;
        return VolatileStoreWithoutBarrier(&m_pMethodTable, pMT);
    }

    // Get approximate method table. This is used by the type loader before the type is fully loaded.
    PTR_MethodTable GetApproxMethodTable(Module * pContainingModule);
#endif // !DACCESS_COMPILE

#ifndef DACCESS_COMPILE
    InterfaceInfo_t(InterfaceInfo_t &right)
    {
        VolatileStoreWithoutBarrier(&m_pMethodTable, VolatileLoadWithoutBarrier(&right.m_pMethodTable));
    }
#else // !DACCESS_COMPILE
private:
    InterfaceInfo_t(InterfaceInfo_t &right);
#endif // !DACCESS_COMPILE
};  // struct InterfaceInfo_t

typedef DPTR(InterfaceInfo_t) PTR_InterfaceInfo;

namespace ClassCompat
{
    struct InterfaceInfo_t;
};

// Data needed when simulating old VTable layout for COM Interop
// This is necessary as the data is saved in MethodDescs and we need
// to simulate different values without copying or changing the existing
// MethodDescs
//
// This will be created in a parallel array to ppMethodDescList and
// ppUnboxMethodDescList in the bmtMethAndFieldDescs structure below
struct InteropMethodTableSlotData
{
    enum
    {
        e_DUPLICATE = 0x0001              // The entry is duplicate
    };

    MethodDesc *pMD;                // The MethodDesc for this slot
    WORD        wSlot;              // The simulated slot value for the MethodDesc
    WORD        wFlags;             // The simulated duplicate value
    MethodDesc *pDeclMD;            // To keep track of MethodImpl's

    void SetDuplicate()
    {
        wFlags |= e_DUPLICATE;
    }

    BOOL IsDuplicate() {
        return ((BOOL)(wFlags & e_DUPLICATE));
    }

    WORD GetSlot() {
        return wSlot;
    }

    void SetSlot(WORD wSlot) {
        this->wSlot = wSlot;
    }
};  // struct InteropMethodTableSlotData

#ifdef FEATURE_COMINTEROP
struct InteropMethodTableData
{
    WORD cVTable;                          // Count of vtable slots
    InteropMethodTableSlotData *pVTable;    // Data for each slot

    WORD cNonVTable;                       // Count of non-vtable slots
    InteropMethodTableSlotData *pNonVTable; // Data for each slot

    WORD            cInterfaceMap;         // Count of interfaces
    ClassCompat::InterfaceInfo_t *
                    pInterfaceMap;         // The interface map

    // Utility methods
    static WORD GetRealMethodDesc(MethodTable *pMT, MethodDesc *pMD);
    static WORD GetSlotForMethodDesc(MethodTable *pMT, MethodDesc *pMD);
    ClassCompat::InterfaceInfo_t* FindInterface(MethodTable *pInterface);
    WORD GetStartSlotForInterface(MethodTable* pInterface);
};

class InteropMethodTableSlotDataMap
{
protected:
    InteropMethodTableSlotData *m_pSlotData;
    DWORD                       m_cSlotData;
    DWORD                       m_iCurSlot;

public:
    InteropMethodTableSlotDataMap(InteropMethodTableSlotData *pSlotData, DWORD cSlotData);
    InteropMethodTableSlotData *GetData(MethodDesc *pMD);
    BOOL Exists(MethodDesc *pMD);

protected:
    InteropMethodTableSlotData *Exists_Helper(MethodDesc *pMD);
    InteropMethodTableSlotData *GetNewEntry();
};  // class InteropMethodTableSlotDataMap
#endif // FEATURE_COMINTEROP

//
// This struct contains cached information on the GUID associated with a type.
//

struct GuidInfo
{
    GUID         m_Guid;                // The actual guid of the type.
    BOOL         m_bGeneratedFromName;  // A boolean indicating if it was generated from the
                                        // name of the type.
};

typedef DPTR(GuidInfo) PTR_GuidInfo;


// GenericsDictInfo is stored at negative offset of the dictionary
struct GenericsDictInfo
{
#ifdef HOST_64BIT
    DWORD m_dwPadding;               // Just to keep the size a multiple of 8
#endif

    // Total number of instantiation dictionaries including inherited ones
    //   i.e. how many instantiated classes (including this one) are there in the hierarchy?
    // See comments about PerInstInfo
    WORD   m_wNumDicts;

    // Number of type parameters (NOT including those of superclasses).
    WORD   m_wNumTyPars;
};  // struct GenericsDictInfo
typedef DPTR(GenericsDictInfo) PTR_GenericsDictInfo;

// These various statics structures exist directly before the MethodTableAuxiliaryData

// Any MethodTable which has static variables has this structure
struct DynamicStaticsInfo;
struct ThreadStaticsInfo;
struct GenericsStaticsInfo;

typedef DPTR(DynamicStaticsInfo) PTR_DynamicStaticsInfo;
typedef DPTR(ThreadStaticsInfo) PTR_ThreadStaticsInfo;
typedef DPTR(GenericsStaticsInfo) PTR_GenericsStaticsInfo;

//
// This struct consolidates the auxiliary parts of the MethodTable
// so that we can layout a variety of variable sized structures and
// access them from the MethodTable with a very small and simple set of
// indirections.
//
struct MethodTableAuxiliaryData
{
    friend class MethodTable;
#if defined(DACCESS_COMPILE)
    friend class NativeImageDumper;
#endif

    enum
    {
        // AS YOU ADD NEW FLAGS PLEASE CONSIDER WHETHER Generics::NewInstantiation NEEDS
        // TO BE UPDATED IN ORDER TO ENSURE THAT METHODTABLES DUPLICATED FOR GENERIC INSTANTIATIONS
        // CARRY THE CORRECT INITIAL FLAGS.

        enum_flag_Initialized               = 0x0001,
        enum_flag_HasCheckedCanCompareBitsOrUseFastGetHashCode  = 0x0002,  // Whether we have checked the overridden Equals or GetHashCode
        enum_flag_CanCompareBitsOrUseFastGetHashCode    = 0x0004,     // Is any field type or sub field type overridden Equals or GetHashCode
        enum_flag_IsTlsIndexAllocated       = 0x0008,
        enum_flag_HasApproxParent           = 0x0010,
        enum_flag_MayHaveOpenInterfaceInInterfaceMap    = 0x0020,
        enum_flag_IsNotFullyLoaded          = 0x0040,
        enum_flag_DependenciesLoaded        = 0x0080,     // class and all dependencies loaded up to CLASS_LOADED_BUT_NOT_VERIFIED

        enum_flag_IsInitError               = 0x0100,
        enum_flag_IsStaticDataAllocated     = 0x0200,     // When this is set, if the class can be marked as initialized without any further code execution it will be.
        enum_flag_HasCheckedStreamOverride  = 0x0400,
        enum_flag_StreamOverriddenRead      = 0x0800,
        enum_flag_StreamOverriddenWrite     = 0x1000,
        enum_flag_EnsuredInstanceActive     = 0x2000,
        // unused enum                      = 0x4000,
        // unused enum                      = 0x8000,
    };
    union
    {
        DWORD      m_dwFlags;                  // Lot of empty bits here.
        struct
        {
            uint16_t m_loFlags;
            int16_t m_offsetToNonVirtualSlots;
        };
    };


    PTR_Module m_pLoaderModule;

    // Non-unloadable context: internal RuntimeType object handle
    // Unloadable context: slot index in LoaderAllocator's pinned table
    RUNTIMETYPEHANDLE m_hExposedClassObject;

#ifdef _DEBUG
    enum
    {
        // The MethodTable is in the right state to be published, and will be inevitably.
        // Currently DEBUG only as it does not affect behavior in any way in a release build
        enum_flagDebug_IsPublished                    = 0x2000,
        enum_flagDebug_ParentMethodTablePointerValid  = 0x4000,
        enum_flagDebug_HasInjectedInterfaceDuplicates = 0x8000,
    };
    DWORD m_dwFlagsDebug;

    // to avoid verify same method table too many times when it's not changing, we cache the GC count
    // on which the method table is verified. When fast GC STRESS is turned on, we only verify the MT if
    // current GC count is bigger than the number. Note most thing which will invalidate a MT will require a
    // GC (like AD unload)
    Volatile<DWORD> m_dwLastVerifedGCCnt;

#ifdef HOST_64BIT
    DWORD m_dwPadding;               // Just to keep the size a multiple of 8
#endif

    // These pointers make it easier to examine the various statics structures in the debugger
    PTR_DynamicStaticsInfo m_debugOnlyDynamicStatics;
    PTR_GenericsStaticsInfo m_debugOnlyGenericStatics;
    PTR_ThreadStaticsInfo m_debugOnlyThreadStatics;
#endif

public:
    inline PTR_Module GetLoaderModule() const
    {
        return m_pLoaderModule;
    }

#ifndef DACCESS_COMPILE
    inline void SetLoaderModule(Module *pModule)
    {
        m_pLoaderModule = pModule;
    }
#endif // DACCESS_COMPILE

#ifdef _DEBUG
    inline BOOL IsParentMethodTablePointerValid() const
    {
        LIMITED_METHOD_DAC_CONTRACT;

        return (m_dwFlagsDebug & enum_flagDebug_ParentMethodTablePointerValid);
    }
    inline void SetParentMethodTablePointerValid()
    {
        LIMITED_METHOD_CONTRACT;

        m_dwFlagsDebug |= enum_flagDebug_ParentMethodTablePointerValid;
    }
#endif

    inline BOOL IsInitError() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (VolatileLoad(&m_dwFlags) & enum_flag_IsInitError);
    }

#ifndef DACCESS_COMPILE
    inline void SetInitError()
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwFlags, (LONG)enum_flag_IsInitError);
    }
#endif

    inline BOOL IsTlsIndexAllocated() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (VolatileLoad(&m_dwFlags) & enum_flag_IsTlsIndexAllocated);
    }

#ifndef DACCESS_COMPILE
    inline void SetIsTlsIndexAllocated()
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwFlags, (LONG)enum_flag_IsTlsIndexAllocated);
    }
#endif

    DWORD* getIsClassInitedFlagAddress()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(enum_flag_Initialized == 1); // This is an assumption in the JIT and in hand-written assembly at this time.
        return &m_dwFlags;
    }

    inline BOOL IsClassInited() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return VolatileLoad(&m_dwFlags) & enum_flag_Initialized;
    }

    inline BOOL IsEnsuredInstanceActive() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return VolatileLoad(&m_dwFlags) & enum_flag_EnsuredInstanceActive;
    }

    inline bool IsClassInitedOrPreinitedDecided(bool *initResult) const
    {
        LIMITED_METHOD_DAC_CONTRACT;

        DWORD dwFlags = VolatileLoad(&m_dwFlags);
        *initResult = m_dwFlags & enum_flag_Initialized;
        return (dwFlags & (enum_flag_IsStaticDataAllocated|enum_flag_Initialized)) != 0;
    }

#ifndef DACCESS_COMPILE
    inline void SetClassInited()
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwFlags, (LONG)enum_flag_Initialized);
    }

    inline void SetEnsuredInstanceActive()
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwFlags, (LONG)enum_flag_EnsuredInstanceActive);
    }
#endif

    inline BOOL IsStaticDataAllocated() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (VolatileLoad(&m_dwFlags) & enum_flag_IsStaticDataAllocated);
    }

#ifndef DACCESS_COMPILE
    inline void SetIsStaticDataAllocated(bool markAsInitedToo)
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwFlags, markAsInitedToo ? (LONG)(enum_flag_IsStaticDataAllocated|enum_flag_Initialized) : (LONG)enum_flag_IsStaticDataAllocated);
    }
#endif

    inline void SetStreamOverrideState(BOOL read, BOOL write)
    {
        LONG streamOverride =
            enum_flag_HasCheckedStreamOverride
            | (read ? enum_flag_StreamOverriddenRead : 0)
            | (write ? enum_flag_StreamOverriddenWrite : 0);
        InterlockedOr((LONG*)&m_dwFlags, streamOverride);
    }

    inline RUNTIMETYPEHANDLE GetExposedClassObjectHandle() const
    {
        LIMITED_METHOD_CONTRACT;
        return m_hExposedClassObject;
    }

    void SetIsNotFullyLoadedForBuildMethodTable()
    {
        LIMITED_METHOD_CONTRACT;

        // Used only during method table initialization - no need for logging or Interlocked Exchange.
        m_dwFlags |= (MethodTableAuxiliaryData::enum_flag_IsNotFullyLoaded |
                      MethodTableAuxiliaryData::enum_flag_HasApproxParent);
    }

    void SetIsRestoredForBuildArrayMethodTable()
    {
        LIMITED_METHOD_CONTRACT;

        // Array's parent is always precise
        m_dwFlags &= ~(MethodTableAuxiliaryData::enum_flag_HasApproxParent);

    }

#ifdef _DEBUG
#ifndef DACCESS_COMPILE
    // Used in DEBUG builds to indicate that the MethodTable is in the right state to be published, and will be inevitably.
    void SetIsPublished()
    {
        LIMITED_METHOD_CONTRACT;
        m_dwFlagsDebug |= (MethodTableAuxiliaryData::enum_flagDebug_IsPublished);
    }
#endif

    // The MethodTable is in the right state to be published, and will be inevitably.
    // Currently DEBUG only as it does not affect behavior in any way in a release build
    bool IsPublished() const
    {
        LIMITED_METHOD_CONTRACT;
        return (VolatileLoad(&m_dwFlagsDebug) & enum_flagDebug_IsPublished);
    }
#endif // _DEBUG

    // The NonVirtualSlots array grows backwards, so this pointer points at just AFTER the first entry in the array
    // To access, use a construct like... GetNonVirtualSlotsArray(pAuxiliaryData)[-(1 + index)]
    static inline PTR_PCODE GetNonVirtualSlotsArray(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return dac_cast<PTR_PCODE>(dac_cast<TADDR>(pAuxiliaryData) + pAuxiliaryData->GetOffsetToNonVirtualSlots());
    }

    inline int16_t GetOffsetToNonVirtualSlots() const
    {
        return m_offsetToNonVirtualSlots;
    }

    inline void SetOffsetToNonVirtualSlots(int16_t offset)
    {
        m_offsetToNonVirtualSlots = offset;
    }

    static inline PTR_DynamicStaticsInfo GetDynamicStaticsInfo(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData);
    static inline PTR_GenericsStaticsInfo GetGenericStaticsInfo(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData);
    static inline PTR_ThreadStaticsInfo GetThreadStaticsInfo(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData);
    inline void SetMayHaveOpenInterfacesInInterfaceMap()
    {
        LIMITED_METHOD_CONTRACT;
        InterlockedOr((LONG*)&m_dwFlags, MethodTableAuxiliaryData::enum_flag_MayHaveOpenInterfaceInInterfaceMap);
    }

    inline bool MayHaveOpenInterfacesInInterfaceMap() const
    {
        return !!(m_dwFlags & MethodTableAuxiliaryData::enum_flag_MayHaveOpenInterfaceInInterfaceMap);
    }
};  // struct MethodTableAuxiliaryData

// All MethodTables which have static variables will have one of these. It contains the pointers necessary to
// find the normal (non-thread) static variables of the type.
struct DynamicStaticsInfo
{
    // The detail of whether or not the class has been initialized is stored in the statics pointers as well as in
    // its normal flag location. This is done so that when getting the statics base for a class, we can get the statics
    // base address and check to see if it is initialized without needing a barrier between reading the flag and reading
    // the static field address.
    static constexpr TADDR ISCLASSNOTINITED = 1;
    static constexpr TADDR ISCLASSNOTINITEDMASK = ISCLASSNOTINITED;
    static constexpr TADDR STATICSPOINTERMASK = ~ISCLASSNOTINITEDMASK;
private:

    void InterlockedSetClassInited(bool isGC)
    {
        TADDR oldVal;
        TADDR oldValFromInterlockedOp;
        TADDR *pAddr = isGC ? &m_pGCStatics : &m_pNonGCStatics;
        do
        {
            oldVal = VolatileLoadWithoutBarrier(pAddr);
            // Mask off the ISCLASSNOTINITED bit
            oldValFromInterlockedOp = InterlockedCompareExchangeT(pAddr, oldVal & STATICSPOINTERMASK, oldVal);
        } while(oldValFromInterlockedOp != oldVal); // We can loop if we happened to allocate the statics pointer in the middle of this operation
    }

public:
    TADDR m_pGCStatics; // Always access through helper methods to properly handle the ISCLASSNOTINITED bit
    TADDR m_pNonGCStatics; // Always access through helper methods to properly handle the ISCLASSNOTINITED bit
    PTR_MethodTable m_pMethodTable;
    PTR_OBJECTREF GetGCStaticsPointer() { TADDR staticsVal = VolatileLoad(&m_pGCStatics); return dac_cast<PTR_OBJECTREF>(staticsVal & STATICSPOINTERMASK); }
    PTR_BYTE GetNonGCStaticsPointer() { TADDR staticsVal = VolatileLoad(&m_pNonGCStatics); return dac_cast<PTR_BYTE>(staticsVal & STATICSPOINTERMASK); }
    PTR_OBJECTREF GetGCStaticsPointerAssumeIsInited() { TADDR staticsVal = m_pGCStatics; _ASSERTE(staticsVal != 0); _ASSERTE((staticsVal & (ISCLASSNOTINITEDMASK)) == 0); return dac_cast<PTR_OBJECTREF>(staticsVal); }
    PTR_BYTE GetNonGCStaticsPointerAssumeIsInited() { TADDR staticsVal = m_pNonGCStatics; _ASSERTE(staticsVal != 0); _ASSERTE((staticsVal & (ISCLASSNOTINITEDMASK)) == 0); return dac_cast<PTR_BYTE>(staticsVal); }
    bool GetIsInitedAndGCStaticsPointerIfInited(PTR_OBJECTREF *ptrResult) { TADDR staticsVal = VolatileLoad(&m_pGCStatics); *ptrResult = dac_cast<PTR_OBJECTREF>(staticsVal); return !(staticsVal & ISCLASSNOTINITED); }
    bool GetIsInitedAndNonGCStaticsPointerIfInited(PTR_BYTE *ptrResult) { TADDR staticsVal = VolatileLoad(&m_pNonGCStatics); *ptrResult = dac_cast<PTR_BYTE>(staticsVal); return !(staticsVal & ISCLASSNOTINITED); }

    // This function sets the pointer portion of a statics pointer. It returns false if the statics value was already set.
    bool InterlockedUpdateStaticsPointer(bool isGC, TADDR newVal, bool isClassInitedByUpdatingStaticPointer)
    {
        TADDR oldVal;
        TADDR oldValFromInterlockedOp;
        TADDR *pAddr = isGC ? &m_pGCStatics : &m_pNonGCStatics;
        do
        {
            oldVal = VolatileLoad(pAddr);

            // Check to see if statics value has already been set
            if ((oldVal & STATICSPOINTERMASK) != 0)
            {
                // If it has, then we don't need to do anything
                return false;
            }

            if (isClassInitedByUpdatingStaticPointer)
            {
                oldValFromInterlockedOp = InterlockedCompareExchangeT(pAddr, newVal, oldVal);
            }
            else
            {
                oldValFromInterlockedOp = InterlockedCompareExchangeT(pAddr, newVal | oldVal, oldVal);
            }

        } while(oldValFromInterlockedOp != oldVal);
        return true;
    }
    void SetClassInited()
    {
        InterlockedSetClassInited(true);
        InterlockedSetClassInited(false);
    }

#ifndef DACCESS_COMPILE
    void Init(MethodTable* pMT)
    {
        m_pGCStatics = ISCLASSNOTINITED;
        m_pNonGCStatics = ISCLASSNOTINITED;
        m_pMethodTable = pMT;
    }
#endif

    PTR_MethodTable GetMethodTable() const { return m_pMethodTable; }
};

/* static */ inline PTR_DynamicStaticsInfo MethodTableAuxiliaryData::GetDynamicStaticsInfo(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData)
{
    return dac_cast<PTR_DynamicStaticsInfo>(dac_cast<TADDR>(pAuxiliaryData) - sizeof(DynamicStaticsInfo));
}

// Any Generic MethodTable which has static variables has this structure. Note that it ends
// with a DynamicStatics structure so that lookups for just DynamicStatics will find that structure
// when looking for statics pointers
// In addition, for simplicity in access, all MethodTables which have a ThreadStaticsInfo have this structure
// but it is unitialized and should not be used if the type is not generic
struct GenericsStaticsInfo
{
    // Pointer to field descs for statics
    PTR_FieldDesc       m_pFieldDescs;

    DynamicStaticsInfo      m_DynamicStatics;
};  // struct GenericsStaticsInfo

/* static */ inline PTR_GenericsStaticsInfo MethodTableAuxiliaryData::GetGenericStaticsInfo(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData)
{
    return dac_cast<PTR_GenericsStaticsInfo>(dac_cast<TADDR>(pAuxiliaryData) - sizeof(GenericsStaticsInfo));
}

// And MethodTable with Thread Statics has this structure. NOTE: This structure includes
// GenericsStatics which may not actually have the m_pFieldDescs filled in if the MethodTable
// is not actually Generic
struct ThreadStaticsInfo
{
    TLSIndex NonGCTlsIndex;
    TLSIndex GCTlsIndex;
    GenericsStaticsInfo m_genericStatics;
    void Init()
    {
        NonGCTlsIndex = TLSIndex::Unallocated();
        GCTlsIndex = TLSIndex::Unallocated();
    }
};

/* static */ inline PTR_ThreadStaticsInfo MethodTableAuxiliaryData::GetThreadStaticsInfo(PTR_Const_MethodTableAuxiliaryData pAuxiliaryData)
{
    return dac_cast<PTR_ThreadStaticsInfo>(dac_cast<TADDR>(pAuxiliaryData) - sizeof(ThreadStaticsInfo));
}

#ifdef UNIX_AMD64_ABI_ITF
inline
SystemVClassificationType CorInfoType2UnixAmd64Classification(CorElementType eeType)
{
    static const SystemVClassificationType toSystemVAmd64ClassificationTypeMap[] = {
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_END
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_VOID
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_BOOLEAN
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_CHAR
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_I1
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_U1
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_I2
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_U2
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_I4
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_U4
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_I8
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_U8
        SystemVClassificationTypeSSE,                   // ELEMENT_TYPE_R4
        SystemVClassificationTypeSSE,                   // ELEMENT_TYPE_R8
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_STRING
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_PTR
        SystemVClassificationTypeIntegerByRef,          // ELEMENT_TYPE_BYREF
        SystemVClassificationTypeStruct,                // ELEMENT_TYPE_VALUETYPE
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_CLASS
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_VAR (type variable)
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_ARRAY
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_GENERICINST
        SystemVClassificationTypeStruct,                // ELEMENT_TYPE_TYPEDBYREF
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_VALUEARRAY_UNSUPPORTED
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_I
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_U
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_R_UNSUPPORTED

        // put the correct type when we know our implementation
        SystemVClassificationTypeInteger,               // ELEMENT_TYPE_FNPTR
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_OBJECT
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_SZARRAY
        SystemVClassificationTypeIntegerReference,      // ELEMENT_TYPE_MVAR

        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_CMOD_REQD
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_CMOD_OPT
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_INTERNAL
        SystemVClassificationTypeUnknown,               // ELEMENT_TYPE_CMOD_INTERNAL
    };

    _ASSERTE(sizeof(toSystemVAmd64ClassificationTypeMap) == ELEMENT_TYPE_MAX);
    _ASSERTE(eeType < (CorElementType) sizeof(toSystemVAmd64ClassificationTypeMap));
    // spot check of the map
    _ASSERTE((SystemVClassificationType)toSystemVAmd64ClassificationTypeMap[ELEMENT_TYPE_I4] == SystemVClassificationTypeInteger);
    _ASSERTE((SystemVClassificationType)toSystemVAmd64ClassificationTypeMap[ELEMENT_TYPE_PTR] == SystemVClassificationTypeInteger);
    _ASSERTE((SystemVClassificationType)toSystemVAmd64ClassificationTypeMap[ELEMENT_TYPE_VALUETYPE] == SystemVClassificationTypeStruct);
    _ASSERTE((SystemVClassificationType)toSystemVAmd64ClassificationTypeMap[ELEMENT_TYPE_TYPEDBYREF] == SystemVClassificationTypeStruct);
    _ASSERTE((SystemVClassificationType)toSystemVAmd64ClassificationTypeMap[ELEMENT_TYPE_BYREF] == SystemVClassificationTypeIntegerByRef);

    return (((unsigned)eeType) < ELEMENT_TYPE_MAX) ? (toSystemVAmd64ClassificationTypeMap[(unsigned)eeType]) : SystemVClassificationTypeUnknown;
};

#define SYSTEMV_EIGHT_BYTE_SIZE_IN_BYTES                    8 // Size of an eightbyte in bytes.
#define SYSTEMV_MAX_NUM_FIELDS_IN_REGISTER_PASSED_STRUCT    16 // Maximum number of fields in struct passed in registers

struct SystemVStructRegisterPassingHelper
{
    SystemVStructRegisterPassingHelper(unsigned int totalStructSize) :
        structSize(totalStructSize),
        eightByteCount(0),
        inEmbeddedStruct(false),
        currentUniqueOffsetField(0),
        largestFieldOffset(-1)
    {
        for (int i = 0; i < CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS; i++)
        {
            eightByteClassifications[i] = SystemVClassificationTypeNoClass;
            eightByteSizes[i] = 0;
            eightByteOffsets[i] = 0;
        }

        // Initialize the work arrays
        for (int i = 0; i < SYSTEMV_MAX_NUM_FIELDS_IN_REGISTER_PASSED_STRUCT; i++)
        {
            fieldClassifications[i] = SystemVClassificationTypeNoClass;
            fieldSizes[i] = 0;
            fieldOffsets[i] = 0;
        }
    }

    // Input state.
    unsigned int                    structSize;

    // These fields are the output; these are what is computed by the classification algorithm.
    unsigned int                    eightByteCount;
    SystemVClassificationType       eightByteClassifications[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS];
    unsigned int                    eightByteSizes[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS];
    unsigned int                    eightByteOffsets[CLR_SYSTEMV_MAX_EIGHTBYTES_COUNT_TO_PASS_IN_REGISTERS];

    // Helper members to track state.
    bool                            inEmbeddedStruct;
    unsigned int                    currentUniqueOffsetField; // A virtual field that could encompass many overlapping fields.
    int                             largestFieldOffset;
    SystemVClassificationType       fieldClassifications[SYSTEMV_MAX_NUM_FIELDS_IN_REGISTER_PASSED_STRUCT];
    unsigned int                    fieldSizes[SYSTEMV_MAX_NUM_FIELDS_IN_REGISTER_PASSED_STRUCT];
    unsigned int                    fieldOffsets[SYSTEMV_MAX_NUM_FIELDS_IN_REGISTER_PASSED_STRUCT];
};

typedef DPTR(SystemVStructRegisterPassingHelper) SystemVStructRegisterPassingHelperPtr;

#endif // UNIX_AMD64_ABI_ITF

#if defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)
// Bitfields for FpStructInRegistersInfo::flags
namespace FpStruct
{
    enum Flags
    {
        // Positions of flags and bitfields
        PosOnlyOne      = 0,
        PosBothFloat    = 1,
        PosFloatInt     = 2,
        PosIntFloat     = 3,
        PosSizeShift1st = 4, // 2 bits
        PosSizeShift2nd = 6, // 2 bits

        UseIntCallConv = 0, // struct is passed according to integer calling convention

        // The flags and bitfields
        OnlyOne          =    1 << PosOnlyOne,      // has only one field, which is floating-point
        BothFloat        =    1 << PosBothFloat,    // has two fields, both are floating-point
        FloatInt         =    1 << PosFloatInt,     // has two fields, 1st is floating and 2nd is integer
        IntFloat         =    1 << PosIntFloat,     // has two fields, 2nd is floating and 1st is integer
        SizeShift1stMask = 0b11 << PosSizeShift1st, // log2(size) of 1st field
        SizeShift2ndMask = 0b11 << PosSizeShift2nd, // log2(size) of 2nd field
        // Note: flags OnlyOne, BothFloat, FloatInt, and IntFloat are mutually exclusive
    };
}

// On RISC-V and LoongArch a struct with up to two non-empty fields, at least one of them floating-point,
// can be passed in registers according to hardware FP calling convention. FpStructInRegistersInfo represents
// passing information for such parameters.
struct FpStructInRegistersInfo
{
    FpStruct::Flags flags;
    uint32_t offset1st;
    uint32_t offset2nd;

    unsigned SizeShift1st() const { return (flags >> FpStruct::PosSizeShift1st) & 0b11; }
    unsigned SizeShift2nd() const { return (flags >> FpStruct::PosSizeShift2nd) & 0b11; }

    unsigned Size1st() const { return 1u << SizeShift1st(); }
    unsigned Size2nd() const { return 1u << SizeShift2nd(); }

    const char* FlagName() const
    {
        switch (flags & (FpStruct::OnlyOne | FpStruct::BothFloat | FpStruct::FloatInt | FpStruct::IntFloat))
        {
            case FpStruct::OnlyOne: return "OnlyOne";
            case FpStruct::BothFloat: return "BothFloat";
            case FpStruct::FloatInt: return "FloatInt";
            case FpStruct::IntFloat: return "IntFloat";
            default: return "?";
        }
    }
};
#endif // defined(TARGET_RISCV64) || defined(TARGET_LOONGARCH64)

//===============================================================================================
//
// GC data appears before the beginning of the MethodTable
//
//@GENERICS:
// Each generic type has a corresponding "generic" method table that serves the following
// purposes:
// * The method table pointer is used as a representative for the generic type e.g. in reflection
// * MethodDescs for methods in the vtable are used for reflection; they should never be invoked.
// Some other information (e.g. BaseSize) makes no sense "generically" but unfortunately gets put in anyway.
//
// Each distinct instantiation of a generic type has its own MethodTable structure.
// However, the EEClass structure can be shared between compatible instantiations e.g. List<string> and List<object>.
// In that case, MethodDescs are also shared between compatible instantiations (but see below about generic methods).
// Hence the vtable entries for MethodTables belonging to such an EEClass are the same.
//
// The non-vtable section of such MethodTables are only present for one of the instantiations (the first one
// requested) as non-vtable entries are never accessed through the vtable pointer of an object so it's always possible
// to ensure that they are accessed through the representative MethodTable that contains them.

// A MethodTable is the fundamental representation of type in the runtime.  It is this structure that
// objects point at (see code:Object).  It holds the size and GC layout of the type, as well as the dispatch table
// for virtual dispach (but not interface dispatch).  There is a distinct method table for every instance of
// a generic type. From here you can get to
//
// * code:EEClass
//
// Important fields
//     * code:MethodTable.m_pEEClass - pointer to the cold part of the type.
//     * code:MethodTable.m_pParentMethodTable - the method table of the parent type.
//
class MethodTableBuilder;
class MethodTable
{
    /************************************
     *  FRIEND FUNCTIONS
     ************************************/
    // DO NOT ADD FRIENDS UNLESS ABSOLUTELY NECESSARY
    // USE ACCESSORS TO READ/WRITE private field members

    // Special access for setting up String object method table correctly
    friend class ClassLoader;
    friend class JIT_TrialAlloc;
    friend class Module;
    friend class EEClass;
    friend class MethodTableBuilder;
    friend class CheckAsmOffsets;
#if defined(DACCESS_COMPILE)
    friend class NativeImageDumper;
#endif

public:
    // Do some sanity checking to make sure it's a method table
    // and not pointing to some random memory.  In particular
    // check that (apart from the special case of instantiated generic types) we have
    // GetCanonicalMethodTable() == this;
    BOOL SanityCheck();

    static void         CallFinalizer(Object *obj);

public:
    PTR_Module GetModule()
    {
        LIMITED_METHOD_CONTRACT;
        return m_pModule;
    }

#ifndef DACCESS_COMPILE
    void SetModule(Module* pModule)
    {
        LIMITED_METHOD_CONTRACT;
        m_pModule = pModule;
    }
#endif

    Assembly *GetAssembly();

    PTR_Module GetModuleIfLoaded();

    // For regular, non-constructed types, GetLoaderModule() == GetModule()
    // For constructed types (e.g. int[], Dict<int[], C>) the hash table through which a type
    // is accessed lives in a "loader module". The rule for determining the loader module must ensure
    // that a type never outlives its loader module with respect to app-domain unloading
    //
    PTR_Module GetLoaderModule();
    PTR_LoaderAllocator GetLoaderAllocator();

    void SetLoaderAllocator(LoaderAllocator* pAllocator);

    MethodTable *LoadEnclosingMethodTable(ClassLoadLevel targetLevel = CLASS_DEPENDENCIES_LOADED);

    LPCWSTR GetPathForErrorMessages();

    //-------------------------------------------------------------------
    // COM INTEROP
    //

#ifdef FEATURE_COMINTEROP
    TypeHandle GetCoClassForInterface();

private:
    TypeHandle SetupCoClassForInterface();

public:
    DWORD IsComClassInterface();

    // Retrieves the COM interface type.
    CorIfaceAttr    GetComInterfaceType();
    void SetComInterfaceType(CorIfaceAttr ItfType);

    OBJECTHANDLE GetOHDelegate();
    void SetOHDelegate (OBJECTHANDLE _ohDelegate);

    CorClassIfaceAttr GetComClassInterfaceType();
    TypeHandle GetDefItfForComClassItf();

    void GetEventInterfaceInfo(MethodTable **ppSrcItfType, MethodTable **ppEvProvType);

    BOOL            IsExtensibleRCW();

    // Helper to get parent class skipping over COM class in
    // the hierarchy
    MethodTable* GetComPlusParentMethodTable();

    DWORD IsComImport();

    // class is a special COM event interface
    int IsComEventItfType();

    //-------------------------------------------------------------------
    // Sparse VTables.   These require a SparseVTableMap in the EEClass in
    // order to record how the CLR's vtable slots map across to COM
    // Interop slots.
    //
    int IsSparseForCOMInterop();

    // COM interop helpers
    // accessors for m_pComData
    ComCallWrapperTemplate *GetComCallWrapperTemplate();
    BOOL                    SetComCallWrapperTemplate(ComCallWrapperTemplate *pTemplate);
#ifdef FEATURE_COMINTEROP_UNMANAGED_ACTIVATION
    ClassFactoryBase       *GetComClassFactory();
    BOOL                    SetComClassFactory(ClassFactoryBase *pFactory);
#endif // FEATURE_COMINTEROP_UNMANAGED_ACTIVATION

    OBJECTREF GetObjCreateDelegate();
    void SetObjCreateDelegate(OBJECTREF orDelegate);

private:
    // This is for COM Interop backwards compatibility
    BOOL InsertComInteropData(InteropMethodTableData *pData);
    InteropMethodTableData *CreateComInteropData(AllocMemTracker *pamTracker);

public:
    InteropMethodTableData *LookupComInteropData();
    // This is the preferable entrypoint, as it will make sure that all
    // parent MT's have their interop data created, and will create and
    // add this MT's data if not available. The caller should make sure that
    // an appropriate lock is taken to prevent duplicates.
    // NOTE: The current caller of this is ComInterop, and it makes calls
    // under its own lock to ensure not duplicates.
    InteropMethodTableData *GetComInteropData();
#endif // !FEATURE_COMINTEROP

    // class is a com object class
    BOOL IsComObjectType()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_ComObject);
    }

    // mark the class type as COM object class
    void SetComObjectType();

    void SetIDynamicInterfaceCastable();
    BOOL IsIDynamicInterfaceCastable();

    void SetIsTrackedReferenceWithFinalizer();
    BOOL IsTrackedReferenceWithFinalizer();

#ifdef FEATURE_TYPEEQUIVALENCE
    // mark the type as opted into type equivalence
    void SetHasTypeEquivalence()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_HasTypeEquivalence);
    }
#endif // FEATURE_TYPEEQUIVALENCE

    // type has opted into type equivalence or is instantiated by/derived from a type that is
    BOOL HasTypeEquivalence()
    {
        LIMITED_METHOD_CONTRACT;
#ifdef FEATURE_TYPEEQUIVALENCE
        return GetFlag(enum_flag_HasTypeEquivalence);
#else
        return FALSE;
#endif // FEATURE_TYPEEQUIVALENCE
    }

    //-------------------------------------------------------------------
    // DYNAMIC ADDITION OF INTERFACES FOR COM INTEROP
    //
    // Support for dynamically added interfaces on extensible RCW's.

#ifdef FEATURE_COMINTEROP
    PTR_InterfaceInfo GetDynamicallyAddedInterfaceMap();
    unsigned GetNumDynamicallyAddedInterfaces();
    BOOL FindDynamicallyAddedInterface(MethodTable *pInterface);
    void AddDynamicInterface(MethodTable *pItfMT);

    BOOL HasDynamicInterfaceMap()
    {
        LIMITED_METHOD_DAC_CONTRACT;

        // All ComObjects except for __ComObject
        // have dynamic Interface maps
        return GetNumInterfaces() > 0
            && IsComObjectType()
            && !ParentEquals(g_pObjectClass)
            && this != g_pBaseCOMObject;
    }
#endif // FEATURE_COMINTEROP

#ifndef DACCESS_COMPILE
    VOID EnsureActive();
    VOID EnsureInstanceActive();
#endif

    CHECK CheckActivated();
    CHECK CheckInstanceActivated();

    //-------------------------------------------------------------------
    // THE DEFAULT CONSTRUCTOR
    //

public:
    BOOL HasDefaultConstructor();
    void SetHasDefaultConstructor();
    WORD GetDefaultConstructorSlot();
    MethodDesc *GetDefaultConstructor(BOOL forceBoxedEntryPoint = FALSE);

    BOOL HasExplicitOrImplicitPublicDefaultConstructor();

public:

    // checks whether the class initialiser should be run on this class, and runs it if necessary
    void CheckRunClassInitThrowing();

    // checks whether or not the non-beforefieldinit class initializers have been run for all types in this type's
    // inheritance hierarchy, and runs them if necessary. This simulates the behavior of running class constructors
    // during object construction.
    void CheckRunClassInitAsIfConstructingThrowing();

#if defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
    static FpStructInRegistersInfo GetFpStructInRegistersInfo(TypeHandle th);
#endif

#if defined(UNIX_AMD64_ABI_ITF)
    // Builds the internal data structures and classifies struct eightbytes for Amd System V calling convention.
    bool ClassifyEightBytes(SystemVStructRegisterPassingHelperPtr helperPtr, unsigned int nestingLevel, unsigned int startOffsetOfStruct, bool isNativeStruct, MethodTable** pByValueClassCache = NULL);
    bool ClassifyEightBytesWithNativeLayout(SystemVStructRegisterPassingHelperPtr helperPtr, unsigned int nestingLevel, unsigned int startOffsetOfStruct, EEClassNativeLayoutInfo const* nativeLayoutInfo);
#endif // defined(UNIX_AMD64_ABI_ITF)

#if !defined(DACCESS_COMPILE)
    void GetNativeSwiftPhysicalLowering(CORINFO_SWIFT_LOWERING* pSwiftLowering, bool useNativeLayout);
#endif

    // Copy m_dwFlags from another method table
    void CopyFlags(MethodTable * pOldMT)
    {
        LIMITED_METHOD_CONTRACT;
        m_dwFlags = pOldMT->m_dwFlags;
        m_dwFlags2 = pOldMT->m_dwFlags2;
    }

    // Init the m_dwFlags field for an array
    void SetIsArray(CorElementType arrayType);

    // mark the class as having its cctor run.
#ifndef DACCESS_COMPILE
    void SetClassInited()
    {
        // This must be before setting the MethodTable level flag, as otherwise there is a race condition where
        // the MethodTable flag is set, which would allows the JIT to generate a call to a helper which assumes
        // the DynamicStaticInfo level flag is set.
        // The other race in the other direction is not a concern, as it can only cause allows reads/write from the static
        // fields, which are effectively inited in any case once we reach this point.
        if (IsDynamicStatics())
        {
            GetDynamicStaticsInfo()->SetClassInited();
        }
        GetAuxiliaryDataForWrite()->SetClassInited();
    }

private:
    bool IsInitedIfStaticDataAllocated();
public:
    // Is the MethodTable current initialized, and/or can the runtime initialize the MethodTable
    // without running any user code. (This function may allocate memory, and may throw OutOfMemory)
    bool IsClassInitedOrPreinited();
#endif

    // Is the MethodTable current known to be initialized
    // If you want to know if it is initialized and allocation/throwing is permitted, call IsClassInitedOrPreinited instead
    BOOL  IsClassInited()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetAuxiliaryDataForWrite()->IsClassInited();
    }

    // Allocate any memory needed for statics, acquire TLSIndex for TLS statics, and check to see if the class can be considered pre-inited, and if so, set the initialized flag
    void EnsureStaticDataAllocated();
    void EnsureTlsIndexAllocated();

    BOOL IsInitError()
    {
        return GetAuxiliaryData()->IsInitError();
    }

#ifndef DACCESS_COMPILE
    void SetClassInitError()
    {
        return GetAuxiliaryDataForWrite()->SetInitError();
    }
#endif

    inline BOOL IsGlobalClass()
    {
        WRAPPER_NO_CONTRACT;
        return (GetTypeDefRid() == RidFromToken(COR_GLOBAL_PARENT_TOKEN));
    }

private:

#if defined(UNIX_AMD64_ABI_ITF)
    void AssignClassifiedEightByteTypes(SystemVStructRegisterPassingHelperPtr helperPtr, unsigned int nestingLevel) const;
    // Builds the internal data structures and classifies struct eightbytes for Amd System V calling convention.
    bool ClassifyEightBytesWithManagedLayout(SystemVStructRegisterPassingHelperPtr helperPtr, unsigned int nestingLevel, unsigned int startOffsetOfStruct, bool isNativeStruct, MethodTable** pByValueClassCache);
#endif // defined(UNIX_AMD64_ABI_ITF)

    // called from CheckRunClassInitThrowing().  The type wasn't marked as
    // inited while we were there, so let's attempt to do the work.
    void  DoRunClassInitThrowing();

    BOOL RunClassInitEx(OBJECTREF *pThrowable);

public:
    //-------------------------------------------------------------------
    // THE CLASS CONSTRUCTOR
    //

    BOOL HasClassConstructor();
    void SetHasClassConstructor();
    WORD GetClassConstructorSlot();

    void AllocateRegularStaticBoxes(OBJECTREF** ppStaticBase);
    void AllocateRegularStaticBox(FieldDesc* pField, Object** boxedStaticHandle);
    static OBJECTREF AllocateStaticBox(MethodTable* pFieldMT, BOOL fPinned, bool canBeFrozen = false);

    void CheckRestore();

    //-------------------------------------------------------------------
    // LOAD LEVEL
    //
    // The load level of a method table is derived from various flag bits
    // See classloadlevel.h for details of each level
    //
    // Level CLASS_LOADED (fully loaded) is special: a type only
    // reaches this level once all of its dependent types are also at
    // this level (generic arguments, parent, interfaces, etc).
    // Fully loading a type to this level is done outside locks, hence the need for
    // a single atomic action that sets the level.
    //
    inline void SetIsFullyLoaded()
    {
        CONTRACTL
        {
            THROWS;
            GC_NOTRIGGER;
            MODE_ANY;
        }
        CONTRACTL_END;

        PRECONDITION(!HasApproxParent());

        InterlockedAnd((LONG*)&GetAuxiliaryDataForWrite()->m_dwFlags, ~MethodTableAuxiliaryData::enum_flag_IsNotFullyLoaded);
    }

    // Equivalent to GetLoadLevel() == CLASS_LOADED
    inline BOOL IsFullyLoaded()
    {
        WRAPPER_NO_CONTRACT;

        return (GetAuxiliaryData()->m_dwFlags & MethodTableAuxiliaryData::enum_flag_IsNotFullyLoaded) == 0;
    }

    inline BOOL CanCompareBitsOrUseFastGetHashCode()
    {
        LIMITED_METHOD_CONTRACT;
        return (GetAuxiliaryData()->m_dwFlags & MethodTableAuxiliaryData::enum_flag_CanCompareBitsOrUseFastGetHashCode);
    }

    // If canCompare is true, this method ensure an atomic operation for setting
    // enum_flag_HasCheckedCanCompareBitsOrUseFastGetHashCode and enum_flag_CanCompareBitsOrUseFastGetHashCode flags.
    inline void SetCanCompareBitsOrUseFastGetHashCode(BOOL canCompare)
    {
        WRAPPER_NO_CONTRACT
        if (canCompare)
        {
            // Set checked and canCompare flags in one interlocked operation.
            InterlockedOr((LONG*)&GetAuxiliaryDataForWrite()->m_dwFlags,
                MethodTableAuxiliaryData::enum_flag_HasCheckedCanCompareBitsOrUseFastGetHashCode | MethodTableAuxiliaryData::enum_flag_CanCompareBitsOrUseFastGetHashCode);
        }
        else
        {
            SetHasCheckedCanCompareBitsOrUseFastGetHashCode();
        }
    }

    inline BOOL HasCheckedCanCompareBitsOrUseFastGetHashCode()
    {
        LIMITED_METHOD_CONTRACT;
        return (GetAuxiliaryData()->m_dwFlags & MethodTableAuxiliaryData::enum_flag_HasCheckedCanCompareBitsOrUseFastGetHashCode);
    }

    inline void SetHasCheckedCanCompareBitsOrUseFastGetHashCode()
    {
        WRAPPER_NO_CONTRACT;
        InterlockedOr((LONG*)&GetAuxiliaryDataForWrite()->m_dwFlags, MethodTableAuxiliaryData::enum_flag_HasCheckedCanCompareBitsOrUseFastGetHashCode);
    }

    inline void SetIsDependenciesLoaded()
    {
        CONTRACTL
        {
            THROWS;
            GC_NOTRIGGER;
            MODE_ANY;
        }
        CONTRACTL_END;

        PRECONDITION(!HasApproxParent());

        InterlockedOr((LONG*)&GetAuxiliaryDataForWrite()->m_dwFlags, MethodTableAuxiliaryData::enum_flag_DependenciesLoaded);
    }

    inline ClassLoadLevel GetLoadLevel()
    {
        LIMITED_METHOD_DAC_CONTRACT;

        DWORD dwFlags = GetAuxiliaryData()->m_dwFlags;

        if (dwFlags & MethodTableAuxiliaryData::enum_flag_IsNotFullyLoaded)
        {
            if (dwFlags & MethodTableAuxiliaryData::enum_flag_HasApproxParent)
                return CLASS_LOAD_APPROXPARENTS;

            if (!(dwFlags & MethodTableAuxiliaryData::enum_flag_DependenciesLoaded))
                return CLASS_LOAD_EXACTPARENTS;

            return CLASS_DEPENDENCIES_LOADED;
        }

        return CLASS_LOADED;
    }

#ifdef _DEBUG
    CHECK CheckLoadLevel(ClassLoadLevel level)
    {
        LIMITED_METHOD_CONTRACT;
        return TypeHandle(this).CheckLoadLevel(level);
    }
#endif


    void DoFullyLoad(Generics::RecursionGraph * const pVisited, const ClassLoadLevel level, DFLPendingList * const pPending, BOOL * const pfBailed,
                     const InstantiationContext * const pInstContext);

    //-------------------------------------------------------------------
    // METHOD TABLES AS TYPE DESCRIPTORS
    //
    // A MethodTable can represeent a type such as "String" or an
    // instantiated type such as "List<String>".
    //

    inline BOOL IsInterface()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_Category_Mask) == enum_flag_Category_Interface;
    }

    void SetIsInterface()
    {
        LIMITED_METHOD_CONTRACT;

        _ASSERTE(GetFlag(enum_flag_Category_Mask) == 0);
        SetFlag(enum_flag_Category_Interface);
    }

    inline BOOL IsSealed();

    inline BOOL IsAbstract();

    BOOL IsExternallyVisible();

    // Get the instantiation for this instantiated type e.g. for Dict<string,int>
    // this would be an array {string,int}
    // If not instantiated, return NULL
    Instantiation GetInstantiation();

    // Get the instantiation for an instantiated type or a pointer to the
    // element type for an array
    Instantiation GetClassOrArrayInstantiation();
    Instantiation GetArrayInstantiation();

    inline BOOL IsIntrinsicType()
    {
        LIMITED_METHOD_DAC_CONTRACT;;
        return GetFlag(enum_flag_IsIntrinsicType);
    }

    inline void SetIsIntrinsicType()
    {
        LIMITED_METHOD_DAC_CONTRACT;;
        SetFlag(enum_flag_IsIntrinsicType);
    }

    // Is this a method table for a generic type instantiation, e.g. List<string>?
    inline BOOL HasInstantiation();

    // Returns true for any class which is either itself a generic
    // instantiation or is derived from a generic
    // instantiation anywhere in it's class hierarchy,
    //
    // e.g. class D : C<int>
    // or class E : D, class D : C<int>
    //
    // Does not return true just because the class supports
    // an instantiated interface type.
    BOOL HasGenericClassInstantiationInHierarchy()
    {
        WRAPPER_NO_CONTRACT;
        return GetNumDicts() != 0;
    }

    // Is this an instantiation of a generic class at its formal
    // type parameters ie. List<T> ?
    inline BOOL IsGenericTypeDefinition();

    BOOL ContainsGenericMethodVariables();

    // When creating an interface map, under some circumstances the
    // runtime will place the special marker type in the interface map instead
    // of the fully loaded type. This is to reduce the amount of type loading
    // performed at process startup.
    //
    // The current rule is that these interfaces can only appear
    // on valuetypes that are not shared generic, and that the special
    // marker type is the open generic type.
    //
    inline bool IsSpecialMarkerTypeForGenericCasting()
    {
        return IsGenericTypeDefinition();
    }

    static const DWORD MaxGenericParametersForSpecialMarkerType = 8;

    static BOOL ComputeContainsGenericVariables(Instantiation inst);

    inline void SetContainsGenericVariables()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_ContainsGenericVariables);
    }

    inline void SetHasVariance()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_HasVariance);
    }

    inline BOOL HasVariance()
    {
        LIMITED_METHOD_CONTRACT;
        return GetFlag(enum_flag_HasVariance);
    }

    // Is this something like List<T> or List<Stack<T>>?
    // List<Blah<T>> only exists for reflection and verification.
    inline DWORD ContainsGenericVariables(BOOL methodVarsOnly = FALSE)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;
        if (methodVarsOnly)
            return ContainsGenericMethodVariables();
        else
            return GetFlag(enum_flag_ContainsGenericVariables);
    }

    BOOL IsByRefLike()
    {
        LIMITED_METHOD_DAC_CONTRACT;;
        return GetFlag(enum_flag_IsByRefLike);
    }

    void SetIsByRefLike()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_IsByRefLike);
    }

    inline BOOL IsTypicalTypeDefinition()
    {
        LIMITED_METHOD_CONTRACT;
        return !HasInstantiation() || IsGenericTypeDefinition();
    }

    PTR_MethodTable GetTypicalMethodTable();

    BOOL HasSameTypeDefAs(MethodTable *pMT);

    //-------------------------------------------------------------------
    // GENERICS & CODE SHARING
    //

    BOOL IsSharedByGenericInstantiations();

    // If this is a "representative" generic MT or a non-generic (regular) MT return true
    inline BOOL IsCanonicalMethodTable();

    // Return the canonical representative MT amongst the set of MT's that share
    // code with the given MT because of generics.
    PTR_MethodTable GetCanonicalMethodTable();

    //-------------------------------------------------------------------
    // Accessing methods by slot number
    //
    // Some of these functions are also currently used to get non-virtual
    // methods, relying on the assumption that they are contiguous.  This
    // is not true for non-virtual methods in generic instantiations, which
    // only live on the canonical method table.

    enum
    {
        NO_SLOT = 0xffff // a unique slot number used to indicate "empty" for fields that record slot numbers
    };

    PCODE GetSlot(UINT32 slotNumber)
    {
        WRAPPER_NO_CONTRACT;
        CONSISTENCY_CHECK(slotNumber < GetNumVtableSlots());

        return *GetSlotPtrRaw(slotNumber);
    }

    // Special-case for when we know that the slot number corresponds
    // to a virtual method.
    inline PCODE GetSlotForVirtual(UINT32 slotNum)
    {
        LIMITED_METHOD_CONTRACT;

        CONSISTENCY_CHECK(slotNum < GetNumVirtuals());
        // Virtual slots live in chunks pointed to by vtable indirections
        return *(GetVtableIndirections()[GetIndexOfVtableIndirection(slotNum)] + GetIndexAfterVtableIndirection(slotNum));
    }

    PTR_PCODE GetSlotPtrRaw(UINT32 slotNum)
    {
        WRAPPER_NO_CONTRACT;
        CONSISTENCY_CHECK(slotNum < GetNumVtableSlots());

        if (slotNum < GetNumVirtuals())
        {
             // Virtual slots live in chunks pointed to by vtable indirections
            return GetVtableIndirections()[GetIndexOfVtableIndirection(slotNum)] + GetIndexAfterVtableIndirection(slotNum);
        }
        else
        {
            // Non-virtual slots < GetNumVtableSlots live before the MethodTableAuxiliaryData. The array grows backwards
            _ASSERTE(HasNonVirtualSlots());
            return MethodTableAuxiliaryData::GetNonVirtualSlotsArray(GetAuxiliaryDataForWrite()) - (1 + (slotNum - GetNumVirtuals()));
        }
    }

    PTR_PCODE GetSlotPtr(UINT32 slotNum)
    {
        WRAPPER_NO_CONTRACT;
        return GetSlotPtrRaw(slotNum);
    }

    void SetSlot(UINT32 slotNum, PCODE slotVal);

    //-------------------------------------------------------------------
    // The VTABLE
    //
    // Rather than the traditional array of code pointers (or "slots") we use a two-level vtable in
    // which slots for virtual methods live in chunks.  Doing so allows the chunks to be shared among
    // method tables (the most common example being between parent and child classes where the child
    // does not override any method in the chunk).  This yields substantial space savings at the fixed
    // cost of one additional indirection for a virtual call.
    //
    // Note that none of this should be visible outside the implementation of MethodTable; all other
    // code continues to refer to a virtual method via the traditional slot number.  This is similar to
    // how we refer to non-virtual methods as having a slot number despite having long ago moved their
    // code pointers out of the vtable.
    //
    // Consider a class where GetNumVirtuals is 5 and (for the sake of the example) assume we break
    // the vtable into chunks of size 3.  The layout would be as follows:
    //
    //   pMT                       chunk 1                   chunk 2
    //   ------------------        ------------------        ------------------
    //   |                |        |      M1()      |        |      M4()      |
    //   |   fixed-size   |        ------------------        ------------------
    //   |   portion of   |        |      M2()      |        |      M5()      |
    //   |   MethodTable  |        ------------------        ------------------
    //   |                |        |      M3()      |
    //   ------------------        ------------------
    //   | ptr to chunk 1 |
    //   ------------------
    //   | ptr to chunk 2 |
    //   ------------------
    //
    // We refer to "ptr to chunk 1" and "ptr to chunk 2" as "indirection slots."
    //
    // The current chunking strategy is independent of class properties; all are of size 8.  Several
    // other strategies were tried, and the only one that has performed better empirically is to begin
    // with a single chunk of size 4 (matching the number of virtuals in System.Object) and then
    // continue with chunks of size 8.  However it was a small improvement and required the run-time
    // helpers listed below to be measurably slower.
    //
    // If you want to change this, you should only need to modify the first four functions below
    // along with any assembly helper that has taken a dependency on the layout.  Currently,
    // those consist of:
    //     JIT_IsInstanceOfInterface
    //     JIT_ChkCastInterface
    //     Transparent proxy stub
    //
    // This layout only applies to the virtual methods in a class (those with slot number below GetNumVirtuals).
    // Non-virtual methods that are in the vtable (those with slot numbers between GetNumVirtuals and
    // GetNumVtableSlots) are laid out in a single chunk pointed to by an optional member.
    // See GetSlotPtrRaw for more details.

    #define VTABLE_SLOTS_PER_CHUNK 8
    #define VTABLE_SLOTS_PER_CHUNK_LOG2 3

    typedef PCODE VTableIndir2_t;
    typedef DPTR(VTableIndir2_t) VTableIndir_t;

    static DWORD GetIndexOfVtableIndirection(DWORD slotNum);

    static DWORD GetStartSlotForVtableIndirection(UINT32 indirectionIndex, DWORD wNumVirtuals);
    static DWORD GetEndSlotForVtableIndirection(UINT32 indirectionIndex, DWORD wNumVirtuals);
    static UINT32 GetIndexAfterVtableIndirection(UINT32 slotNum);
    static UINT32 IndexAfterVtableIndirectionToSlot(UINT32 slotNum);
    static DWORD GetNumVtableIndirections(DWORD wNumVirtuals);
    DPTR(VTableIndir_t) GetVtableIndirections();
    DWORD GetNumVtableIndirections();

    class VtableIndirectionSlotIterator
    {
        friend class MethodTable;

    private:
        DPTR(VTableIndir_t) m_pSlot;
        DWORD m_i;
        DWORD m_count;
        PTR_MethodTable m_pMT;

        VtableIndirectionSlotIterator(MethodTable *pMT);
        VtableIndirectionSlotIterator(MethodTable *pMT, DWORD index);

    public:
        BOOL Next();
        BOOL Finished();
        DWORD GetIndex();
        DWORD GetOffsetFromMethodTable();
        DPTR(VTableIndir2_t) GetIndirectionSlot();

#ifndef DACCESS_COMPILE
        void SetIndirectionSlot(DPTR(VTableIndir2_t) pChunk);
#endif

        DWORD GetStartSlot();
        DWORD GetEndSlot();
        DWORD GetNumSlots();
        DWORD GetSize();
    };  // class VtableIndirectionSlotIterator

    VtableIndirectionSlotIterator IterateVtableIndirectionSlots();
    VtableIndirectionSlotIterator IterateVtableIndirectionSlotsFrom(DWORD index);

    inline BOOL HasNonVirtualSlots()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetNumNonVirtualSlots() != 0;
    }

    inline unsigned GetNonVirtualSlotsArraySize()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetNumNonVirtualSlots() * sizeof(PCODE);
    }

    inline WORD GetNumNonVirtualSlots();

    inline BOOL HasVirtualStaticMethods() const;
    inline void SetHasVirtualStaticMethods();

    void VerifyThatAllVirtualStaticMethodsAreImplemented();

    inline WORD GetNumVirtuals()
    {
        LIMITED_METHOD_DAC_CONTRACT;

        return m_wNumVirtuals;
    }

    inline void SetNumVirtuals (WORD wNumVtableSlots)
    {
        LIMITED_METHOD_CONTRACT;
        m_wNumVirtuals = wNumVtableSlots;
    }

    unsigned GetNumParentVirtuals()
    {
        LIMITED_METHOD_CONTRACT;
        if (IsInterface()) {
            return 0;
        }
        MethodTable *pMTParent = GetParentMethodTable();
        return pMTParent == NULL ? 0 : pMTParent->GetNumVirtuals();
    }

    #define SIZEOF__MethodTable_ (0x10 + (6 INDEBUG(+1)) * TARGET_POINTER_SIZE)

    static inline DWORD GetVtableOffset()
    {
        LIMITED_METHOD_DAC_CONTRACT;

        return SIZEOF__MethodTable_;
    }

    // Return total methods: virtual, static, and instance method slots.
    WORD GetNumMethods();

    // Return number of slots in this methodtable. This is just an information about the layout of the methodtable, it should not be used
    // for functionality checks. Do not confuse with GetNumVirtuals()!
    WORD GetNumVtableSlots()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetNumVirtuals() + GetNumNonVirtualSlots();
    }

    //-------------------------------------------------------------------
    // Slots <-> the MethodDesc associated with the slot.
    //

#ifndef DACCESS_COMPILE
    // Get the MethodDesc that implements a given slot
    // NOTE: Since this may fill in the slot with a temporary entrypoint if that hasn't happened
    //       yet, when writing asserts, GetMethodDescForSlot_NoThrow should be used to avoid
    //       the presence of an assert hiding bugs.
    MethodDesc* GetMethodDescForSlot(DWORD slot);
#endif

    // This api produces the same result as GetMethodDescForSlot, but it uses a variation on the
    // algorithm that does not allocate a temporary entrypoint for the slot if it doesn't exist.
    MethodDesc* GetMethodDescForSlot_NoThrow(DWORD slot);

    static MethodDesc*  GetMethodDescForSlotAddress(PCODE addr, BOOL fSpeculative = FALSE);

    PCODE GetRestoredSlot(DWORD slot);

    // Returns MethodTable that GetRestoredSlot get its values from
    MethodTable * GetRestoredSlotMT(DWORD slot);

    // Used to map methods on the same slot between instantiations.
    MethodDesc * GetParallelMethodDesc(MethodDesc * pDefMD, AsyncVariantLookup asyncVariantLookup = (AsyncVariantLookup)0);

    //-------------------------------------------------------------------
    // BoxedEntryPoint MethodDescs.
    //
    // Virtual methods on structs have BoxedEntryPoint method descs in their vtable.
    // See also notes for MethodDesc::FindOrCreateAssociatedMethodDesc.  You should
    // probably be using that function if you need to map between unboxing
    // stubs and non-unboxing stubs.

    MethodDesc* GetBoxedEntryPointMD(MethodDesc *pMD);

    MethodDesc* GetUnboxedEntryPointMD(MethodDesc *pMD);
    MethodDesc* GetExistingUnboxedEntryPointMD(MethodDesc *pMD);

    //-------------------------------------------------------------------
    // FIELD LAYOUT, OBJECT SIZE ETC.
    //

    inline BOOL HasLayout();

    inline EEClassLayoutInfo* GetLayoutInfo();

    EEClassNativeLayoutInfo const* GetNativeLayoutInfo();

    EEClassNativeLayoutInfo const* EnsureNativeLayoutInfoInitialized();

    inline BOOL IsBlittable();

    inline BOOL IsManagedSequential();

    inline BOOL HasExplicitSize();

    inline BOOL IsAutoLayoutOrHasAutoLayoutField();

    // Only accurate on types which are not auto layout
    inline BOOL IsInt128OrHasInt128Fields();

    UINT32 GetNativeSize();

    DWORD           GetBaseSize()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return(m_BaseSize);
    }

    void            SetBaseSize(DWORD baseSize)
    {
        LIMITED_METHOD_CONTRACT;
        m_BaseSize = baseSize;
    }

    BOOL            IsStringOrArray() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return HasComponentSize();
    }

    BOOL IsString()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return HasComponentSize() && !IsArray() && RawGetComponentSize() == 2;
    }

    BOOL            HasComponentSize() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_HasComponentSize);
    }

    // returns random combination of flags if this doesn't have a component size
    WORD            RawGetComponentSize()
    {
        LIMITED_METHOD_DAC_CONTRACT;
#if BIGENDIAN
        return *((WORD*)&m_dwFlags + 1);
#else // !BIGENDIAN
        return *(WORD*)&m_dwFlags;
#endif // !BIGENDIAN
    }

    // returns 0 if this doesn't have a component size

    // The component size is actually 16-bit WORD, but this method is returning SIZE_T to ensure
    // that SIZE_T is used everywhere for object size computation. It is necessary to support
    // objects bigger than 2GB.
    SIZE_T          GetComponentSize()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return HasComponentSize() ? RawGetComponentSize() : 0;
    }

    void SetComponentSize(WORD wComponentSize)
    {
        LIMITED_METHOD_CONTRACT;
        // it would be nice to assert here that this is either a string
        // or an array, but how do we know.
        //
        // it would also be nice to assert that the component size is > 0,
        // but that would not hold for array's of System.Void and generic type parameters
        SetFlag(enum_flag_HasComponentSize);
        m_dwFlags = (m_dwFlags & ~0xFFFF) | wComponentSize;
    }

    inline WORD GetNumInstanceFields();

    inline WORD GetNumStaticFields();

    inline WORD GetNumThreadStaticFields();

    // Note that for value types GetBaseSize returns the size of instance fields for
    // a boxed value, and GetNumInstanceFieldsBytes for an unboxed value.
    // We place methods like these on MethodTable primarily so we can choose to cache
    // the information within MethodTable, and so less code manipulates EEClass
    // objects directly, because doing so can lead to bugs related to generics.
    //
    inline DWORD GetNumInstanceFieldBytes();

    // Returns the size of the instance fields for a value type, in bytes when
    // the type is known to contain GC pointers. This takes advantage of the detail
    // that if the type contains GC pointers, the size of the instance fields is aligned
    // to pointer sized boundaries. This is only faster if we already have some reason
    // to have checked for ContainsGCPointers.
    inline DWORD GetNumInstanceFieldBytesIfContainsGCPointers()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(ContainsGCPointers());
        _ASSERTE(GetBaseSize() - (DWORD)(2 * sizeof(TADDR)) == GetNumInstanceFieldBytes());
        return GetBaseSize() - (DWORD)(2 * sizeof(TADDR));
    }

    int GetFieldAlignmentRequirement();

    inline WORD GetNumIntroducedInstanceFields();

    BOOL           ContainsGCPointers()
    {
        LIMITED_METHOD_CONTRACT;
        return !!GetFlag(enum_flag_ContainsGCPointers);
    }

    BOOL            Collectible()
    {
        LIMITED_METHOD_CONTRACT;
#ifdef FEATURE_COLLECTIBLE_TYPES
        return GetFlag(enum_flag_Collectible);
#else
        return FALSE;
#endif
    }

    BOOL            ContainsGCPointersOrCollectible()
    {
        LIMITED_METHOD_CONTRACT;
        return GetFlag(enum_flag_ContainsGCPointers) || GetFlag(enum_flag_Collectible);
    }

    OBJECTHANDLE    GetLoaderAllocatorObjectHandle();
    NOINLINE BYTE *GetLoaderAllocatorObjectForGC();

    BOOL            IsNotTightlyPacked();

    BOOL            IsAllGCPointers();

    void SetContainsGCPointers()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_ContainsGCPointers);
    }

#ifdef FEATURE_64BIT_ALIGNMENT
    inline bool RequiresAlign8()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return !!GetFlag(enum_flag_RequiresAlign8);
    }

    inline void SetRequiresAlign8()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_RequiresAlign8);
    }
#endif // FEATURE_64BIT_ALIGNMENT

    //-------------------------------------------------------------------
    // FIELD DESCRIPTORS
    //
    // Most of this API still lives on EEClass.
    //
    // ************************************ WARNING *************
    // **   !!!!INSTANCE FIELDDESCS ARE REPRESENTATIVES!!!!!   **
    // ** THEY ARE SHARED BY COMPATIBLE GENERIC INSTANTIATIONS **
    // ************************************ WARNING *************

    // This goes straight to the EEClass
    // Careful about using this method. If it's possible that fields may have been added via EnC, then
    // must use the FieldDescIterator as any fields added via EnC won't be in the raw list
    PTR_FieldDesc GetApproxFieldDescListRaw();

    // This returns a type-exact FieldDesc for a static field, but may still return a representative
    // for a non-static field.
    PTR_FieldDesc GetFieldDescByIndex(DWORD fieldIndex);

    DWORD GetIndexForFieldDesc(FieldDesc *pField);

    inline bool HasPreciseInitCctors()
    {
        LIMITED_METHOD_CONTRACT;
        return !!GetFlag(enum_flag_HasPreciseInitCctors);
    }

    inline void SetHasPreciseInitCctors()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_HasPreciseInitCctors);
    }

#if defined(FEATURE_HFA)
    inline bool IsHFA()
    {
        LIMITED_METHOD_CONTRACT;
        return !!GetFlag(enum_flag_IsHFA);
    }

    inline void SetIsHFA()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_IsHFA);
    }
#else // !FEATURE_HFA
    bool IsHFA();
#endif // FEATURE_HFA

    // Returns the size in bytes of this type if it is a HW vector type; 0 otherwise.
    int GetVectorSize();

    // Get the HFA type. This is supported both with FEATURE_HFA, in which case it
    // depends on the cached bit on the class, or without, in which case it is recomputed
    // for each invocation.
    CorInfoHFAElemType GetHFAType();
    // The managed and unmanaged HFA type can differ for types with layout. The following two methods return the unmanaged HFA type.
    bool IsNativeHFA();
    CorInfoHFAElemType GetNativeHFAType();

#ifdef UNIX_AMD64_ABI
    inline bool IsRegPassedStruct()
    {
        LIMITED_METHOD_CONTRACT;
        return !!GetFlag(enum_flag_IsRegStructPassed);
    }

    inline void SetRegPassedStruct()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_IsRegStructPassed);
    }
#else
    inline bool IsRegPassedStruct()
    {
        return false;
    }
#endif

#ifdef FEATURE_64BIT_ALIGNMENT
    // Returns true iff the native view of this type requires 64-bit alignment.
    bool NativeRequiresAlign8();
#endif // FEATURE_64BIT_ALIGNMENT

    //-------------------------------------------------------------------
    // PARENT INTERFACES
    //
    unsigned GetNumInterfaces()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_wNumInterfaces;
    }

    //-------------------------------------------------------------------
    // CASTING
    //
    BOOL CanCastToInterface(MethodTable *pTargetMT, TypeHandlePairList *pVisited = NULL);
    BOOL CanCastToClass(MethodTable *pTargetMT, TypeHandlePairList *pVisited = NULL);
    BOOL CanCastTo(MethodTable* pTargetMT, TypeHandlePairList *pVisited);
    BOOL ArraySupportsBizarreInterface(MethodTable* pInterfaceMT, TypeHandlePairList* pVisited);
    BOOL ArrayIsInstanceOf(MethodTable* pTargetMT, TypeHandlePairList* pVisited);

    BOOL CanCastByVarianceToInterfaceOrDelegate(MethodTable* pTargetMT, TypeHandlePairList* pVisited, MethodTable* pMTInterfaceMapOwner = NULL);

    // The inline part of equivalence check.
#ifndef DACCESS_COMPILE
    FORCEINLINE BOOL IsEquivalentTo(MethodTable *pOtherMT COMMA_INDEBUG(TypeHandlePairList *pVisited = NULL));

#ifdef FEATURE_TYPEEQUIVALENCE
    // This method is public so that TypeHandle has direct access to it
    BOOL IsEquivalentTo_Worker(MethodTable *pOtherMT COMMA_INDEBUG(TypeHandlePairList *pVisited));      // out-of-line part, SO tolerant
private:
    BOOL IsEquivalentTo_WorkerInner(MethodTable *pOtherMT COMMA_INDEBUG(TypeHandlePairList *pVisited)); // out-of-line part, SO intolerant
#endif // FEATURE_TYPEEQUIVALENCE
#endif

public:
    //-------------------------------------------------------------------
    // THE METHOD TABLE PARENT (SUPERCLASS/BASE CLASS)
    //
    BOOL HasApproxParent()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (GetAuxiliaryData()->m_dwFlags & MethodTableAuxiliaryData::enum_flag_HasApproxParent) != 0;
    }
    inline void SetHasExactParent()
    {
        WRAPPER_NO_CONTRACT;
        InterlockedAnd((LONG*)&GetAuxiliaryDataForWrite()->m_dwFlags, ~MethodTableAuxiliaryData::enum_flag_HasApproxParent);
    }


    // Caller must know that the parent method table is not an encoded fixup
    inline PTR_MethodTable GetParentMethodTable()
    {
        LIMITED_METHOD_DAC_CONTRACT;

        PRECONDITION(IsParentMethodTablePointerValid());
        return m_pParentMethodTable;
    }

#ifndef DACCESS_COMPILE
    inline MethodTable ** GetParentMethodTableValuePtr()
    {
        LIMITED_METHOD_CONTRACT;
        return &m_pParentMethodTable;
    }
#endif // !DACCESS_COMPILE

    // Is the parent method table pointer equal to the given argument?
    BOOL ParentEquals(PTR_MethodTable pMT)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        PRECONDITION(IsParentMethodTablePointerValid());
        return GetParentMethodTable() == pMT;
    }

#ifdef _DEBUG
    BOOL IsParentMethodTablePointerValid();
#endif

#ifndef DACCESS_COMPILE
    void SetParentMethodTable (MethodTable *pParentMethodTable)
    {
        LIMITED_METHOD_CONTRACT;
        m_pParentMethodTable = pParentMethodTable;
#ifdef _DEBUG
        GetAuxiliaryDataForWrite()->SetParentMethodTablePointerValid();
#endif
    }
#endif // !DACCESS_COMPILE
    MethodTable * GetMethodTableMatchingParentClass(MethodTable * pWhichParent);
    Instantiation GetInstantiationOfParentClass(MethodTable *pWhichParent);

    //-------------------------------------------------------------------
    // THE  EEClass (Possibly shared between instantiations!).
    //
    // Note that it is not generally the case that GetClass.GetMethodTable() == t.

    PTR_EEClass GetClass();

    PTR_EEClass GetClassWithPossibleAV();

    BOOL ValidateWithPossibleAV();

    BOOL IsClassPointerValid();

    static UINT32 GetOffsetOfFlags()
    {
        LIMITED_METHOD_CONTRACT;
        return offsetof(MethodTable, m_dwFlags);
    }

    static UINT32 GetIfArrayThenSzArrayFlag()
    {
        LIMITED_METHOD_CONTRACT;
        return enum_flag_Category_IfArrayThenSzArray;
    }

    //-------------------------------------------------------------------
    // CONSTRUCTION
    //
    // Do not call the following at any time except when creating a method table.
    // One day we will have proper constructors for method tables and all these
    // will disappear.
#ifndef DACCESS_COMPILE
    inline void SetClass(EEClass *pClass)
    {
        LIMITED_METHOD_CONTRACT;
        m_pEEClass = pClass;
    }

    inline void SetCanonicalMethodTable(MethodTable * pMT)
    {
        m_pCanonMT = (TADDR)pMT | MethodTable::UNION_METHODTABLE;
    }
#endif

    inline void SetHasInstantiation(BOOL fTypicalInstantiation, BOOL fSharedByGenericInstantiations);

    //-------------------------------------------------------------------
    // INTERFACE IMPLEMENTATION
    //
 public:
    // Faster force-inlined version of ImplementsInterface
    BOOL ImplementsInterfaceInline(MethodTable *pInterface);

    BOOL ImplementsInterface(MethodTable *pInterface);
    BOOL ImplementsEquivalentInterface(MethodTable *pInterface);

    MethodDesc *GetMethodDescForInterfaceMethod(TypeHandle ownerType, MethodDesc *pInterfaceMD, BOOL throwOnConflict);
    MethodDesc *GetMethodDescForInterfaceMethod(MethodDesc *pInterfaceMD, BOOL throwOnConflict); // You can only use this one for non-generic interfaces

    //-------------------------------------------------------------------
    // INTERFACE MAP.
    //

    inline PTR_InterfaceInfo GetInterfaceMap();

#ifndef DACCESS_COMPILE
    void SetInterfaceMap(WORD wNumInterfaces, InterfaceInfo_t* iMap);
#endif

    inline int HasInterfaceMap()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (m_wNumInterfaces != 0);
    }

    // Where possible, use this iterator over the interface map instead of accessing the map directly
    // That way we can easily change the implementation of the map
    class InterfaceMapIterator
    {
        friend class MethodTable;

    private:
        PTR_InterfaceInfo m_pMap;
        DWORD m_i;
        DWORD m_count;

        InterfaceMapIterator(MethodTable *pMT)
          : m_pMap(pMT->GetInterfaceMap()),
            m_i((DWORD) -1),
            m_count(pMT->GetNumInterfaces())
        {
            WRAPPER_NO_CONTRACT;
        }

        InterfaceMapIterator(MethodTable *pMT, DWORD index)
          : m_pMap(pMT->GetInterfaceMap() + index),
            m_i(index),
            m_count(pMT->GetNumInterfaces())
        {
            WRAPPER_NO_CONTRACT;
            CONSISTENCY_CHECK(index >= 0 && index < m_count);
        }

    public:
        InterfaceInfo_t* GetInterfaceInfo()
        {
            LIMITED_METHOD_CONTRACT;
            return m_pMap;
        }

        // Move to the next item in the map, returning TRUE if an item
        // exists or FALSE if we've run off the end
        inline BOOL Next()
        {
            LIMITED_METHOD_CONTRACT;
            PRECONDITION(!Finished());
            if (m_i != (DWORD) -1)
                m_pMap++;
            return (++m_i < m_count);
        }

        // Have we iterated over all of the items?
        BOOL Finished()
        {
            return (m_i == m_count);
        }

#ifndef DACCESS_COMPILE
        // Get the interface at the current position. This GetInterfaceMethod
        // will ensure that the exact correct instantiation of the interface
        // is found, even if the MethodTable in the interface map is the generic
        // approximation
        PTR_MethodTable GetInterface(MethodTable* pMTOwner, ClassLoadLevel loadLevel = CLASS_LOADED);
#endif

        // Get the interface at the current position, with whatever its normal load level is
        inline PTR_MethodTable GetInterfaceApprox()
        {
            CONTRACT(PTR_MethodTable)
            {
                GC_NOTRIGGER;
                NOTHROW;
                SUPPORTS_DAC;
                PRECONDITION(m_i != (DWORD) -1 && m_i < m_count);
                POSTCONDITION(CheckPointer(RETVAL));
            }
            CONTRACT_END;

            RETURN (m_pMap->GetMethodTable());
        }

        inline bool CurrentInterfaceMatches(MethodTable* pMTOwner, MethodTable* pMT)
        {
            CONTRACT(bool)
            {
                GC_NOTRIGGER;
                NOTHROW;
                SUPPORTS_DAC;
                PRECONDITION(m_i != (DWORD) -1 && m_i < m_count);
            }
            CONTRACT_END;

            MethodTable *pCurrentMethodTable = m_pMap->GetMethodTable();

            bool exactMatch = pCurrentMethodTable == pMT;
            if (!exactMatch)
            {
                if (pCurrentMethodTable->HasSameTypeDefAs(pMT) &&
                    pMT->HasInstantiation() &&
                    pCurrentMethodTable->IsSpecialMarkerTypeForGenericCasting() &&
                    !pMTOwner->GetAuxiliaryData()->MayHaveOpenInterfacesInInterfaceMap() &&
                    pMT->GetInstantiation().ContainsAllOneType(pMTOwner))
                {
                    exactMatch = true;
#ifndef DACCESS_COMPILE
                    // We match exactly, and have an actual pMT loaded. Insert
                    // the searched for interface if it is fully loaded, so that
                    // future checks are more efficient
                    if (pMT->IsFullyLoaded())
                        SetInterface(pMT);
#endif
                }
            }

            RETURN (exactMatch);
        }

        inline bool HasSameTypeDefAs(MethodTable* pMT)
        {
            CONTRACT(bool)
            {
                GC_NOTRIGGER;
                NOTHROW;
                SUPPORTS_DAC;
                PRECONDITION(m_i != (DWORD) -1 && m_i < m_count);
            }
            CONTRACT_END;

            RETURN (m_pMap->GetMethodTable()->HasSameTypeDefAs(pMT));
        }

#ifndef DACCESS_COMPILE
        void SetInterface(MethodTable *pMT)
        {
            WRAPPER_NO_CONTRACT;
            m_pMap->SetMethodTable(pMT);
        }
#endif

        DWORD GetIndex()
        {
            LIMITED_METHOD_CONTRACT;
            return m_i;
        }
    };  // class InterfaceMapIterator

    // Create a new iterator over the interface map
    // The iterator starts just before the first item in the map
    InterfaceMapIterator IterateInterfaceMap()
    {
        WRAPPER_NO_CONTRACT;
        return InterfaceMapIterator(this);
    }

    // Create a new iterator over the interface map, starting at the index specified
    InterfaceMapIterator IterateInterfaceMapFrom(DWORD index)
    {
        WRAPPER_NO_CONTRACT;
        return InterfaceMapIterator(this, index);
    }

    //-------------------------------------------------------------------
    // ADDITIONAL INTERFACE MAP DATA
    //

    // We store extra info (flag bits) for interfaces implemented on this MethodTable in a separate optional
    // location for better data density (if we put them in the interface map directly data alignment could
    // have us using 32 or even 64 bits to represent a single boolean value). Currently the only flag we
    // persist is IsDeclaredOnClass (was the interface explicitly declared by this class).

    // Currently we always store extra info whenever we have an interface map (in the future you could imagine
    // this being limited to those scenarios in which at least one of the interfaces has a non-default value
    // for a flag).
    inline BOOL HasExtraInterfaceInfo()
    {
        SUPPORTS_DAC;
        return HasInterfaceMap();
    }

    // Count of interfaces that can have their extra info stored inline in the optional data structure itself
    // (once the interface count exceeds this limit the optional data slot will instead point to a buffer with
    // the information).
    enum { kInlinedInterfaceInfoThreshold = sizeof(TADDR) * 8 };

    // Calculate how many bytes of storage will be required to track additional information for interfaces.
    // This will be zero if there are no interfaces, but can also be zero for small numbers of interfaces as
    // well, and callers should be ready to handle this.
    static SIZE_T GetExtraInterfaceInfoSize(DWORD cInterfaces);

    // Called after GetExtraInterfaceInfoSize above to setup a new MethodTable with the additional memory to
    // track extra interface info. If there are a non-zero number of interfaces implemented on this class but
    // GetExtraInterfaceInfoSize() returned zero, this call must still be made (with a NULL argument).
    void InitializeExtraInterfaceInfo(PVOID pInfo);

#ifdef DACCESS_COMPILE
    void EnumMemoryRegionsForExtraInterfaceInfo();
#endif // DACCESS_COMPILE

    // For the given interface in the map (specified via map index) mark the interface as declared explicitly
    // on this class. This is not legal for dynamically added interfaces (as used by RCWs).
    void SetInterfaceDeclaredOnClass(DWORD index);

    // For the given interface in the map (specified via map index) return true if the interface was declared
    // explicitly on this class.
    bool IsInterfaceDeclaredOnClass(DWORD index);

    //-------------------------------------------------------------------
    // VIRTUAL/INTERFACE CALL RESOLUTION
    //
    // These should probably go in method.hpp since they don't have
    // much to do with method tables per se.
    //

    // get the method desc given the interface method desc
    static MethodDesc *GetMethodDescForInterfaceMethodAndServer(TypeHandle ownerType, MethodDesc *pItfMD, OBJECTREF *pServer);

#ifdef FEATURE_COMINTEROP
    // get the method desc given the interface method desc on a COM implemented server (if fNullOk is set then NULL is an allowable return value)
    MethodDesc *GetMethodDescForComInterfaceMethod(MethodDesc *pItfMD, bool fNullOk);
#endif // FEATURE_COMINTEROP

    // Resolve virtual static interface method pInterfaceMD on this type.
    //
    // Specify allowNullResult to return NULL instead of throwing if the there is no implementation
    // Specify verifyImplemented to verify that there is a match, but do not actually return a final usable MethodDesc
    // Specify allowVariantMatches to permit generic interface variance
    // Specify uniqueResolution to store the flag saying whether the resolution was unambiguous;
    // when NULL, throw an AmbiguousResolutionException upon hitting ambiguous SVM resolution.
    // The 'level' parameter specifies the load level for the class containing the resolved MethodDesc.
    MethodDesc *ResolveVirtualStaticMethod(
        MethodTable* pInterfaceType,
        MethodDesc* pInterfaceMD,
        ResolveVirtualStaticMethodFlags resolveVirtualStaticMethodFlags,
        BOOL *uniqueResolution = NULL,
        ClassLoadLevel level = CLASS_LOADED);

    // Try a partial resolve of the constraint call, up to generic code sharing.
    //
    // Note that this will not necessarily resolve the call exactly, since we might be compiling
    // shared generic code - it may just resolve it to a candidate suitable for
    // JIT compilation, and require a runtime lookup for the actual code pointer
    // to call.
    //
    // Return NULL if the call could not be resolved, e.g. because it is invoked
    // on a type that inherits the implementation of the method from System.Object
    // or System.ValueType.
    //
    // Always returns an unboxed entry point with a uniform calling convention.
    MethodDesc * TryResolveConstraintMethodApprox(
        TypeHandle   ownerType,
        MethodDesc * pMD,
        BOOL *       pfForceUseRuntimeLookup = NULL);

    //-------------------------------------------------------------------
    // CONTRACT IMPLEMENTATIONS
    //

    inline BOOL HasDispatchMap()
    {
        WRAPPER_NO_CONTRACT;
        return GetDispatchMap() != NULL;
    }

    PTR_DispatchMap GetDispatchMap();

    inline BOOL HasDispatchMapSlot()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_HasDispatchMapSlot);
    }

protected:
    BOOL FindEncodedMapDispatchEntry(UINT32 typeID,
                                     UINT32 slotNumber,
                                     DispatchMapEntry *pEntry);

    BOOL FindIntroducedImplementationTableDispatchEntry(UINT32 slotNumber,
                                                        DispatchMapEntry *pEntry,
                                                        BOOL fVirtualMethodsOnly);

    BOOL FindDispatchEntryForCurrentType(UINT32 typeID,
                                         UINT32 slotNumber,
                                         DispatchMapEntry *pEntry);

    BOOL FindDispatchEntry(UINT32 typeID,
                           UINT32 slotNumber,
                           DispatchMapEntry *pEntry);

private:
    BOOL FindDispatchImpl(
        UINT32         typeID,
        UINT32         slotNumber,
        DispatchSlot * pImplSlot,
        BOOL           throwOnConflict);

public:
#ifndef DACCESS_COMPILE
    BOOL FindDefaultInterfaceImplementation(
        MethodDesc *pInterfaceMD,
        MethodTable *pObjectMT,
        MethodDesc **ppDefaultMethod,
        FindDefaultInterfaceImplementationFlags findDefaultImplementationFlags,
        ClassLoadLevel level = CLASS_LOADED);
#endif // DACCESS_COMPILE

    DispatchSlot FindDispatchSlot(UINT32 typeID, UINT32 slotNumber, BOOL throwOnConflict);

    // You must use the second of these two if there is any chance the pMD is a method
    // on a generic interface such as IComparable<T> (which it normally can be).  The
    // ownerType is used to provide an exact qualification in the case the pMD is
    // a shared method descriptor.
    DispatchSlot FindDispatchSlotForInterfaceMD(MethodDesc *pMD, BOOL throwOnConflict);
    DispatchSlot FindDispatchSlotForInterfaceMD(TypeHandle ownerType, MethodDesc *pMD, BOOL throwOnConflict);

    MethodDesc *ReverseInterfaceMDLookup(UINT32 slotNumber);

    // Lookup, does not assign if not already done.
    UINT32 LookupTypeID();
    // Lookup, will assign ID if not already done.
    UINT32 GetTypeID();


    // Will return either the dispatch map type. May trigger type loader in order to get
    // exact result.
    MethodTable *LookupDispatchMapType(DispatchMapTypeID typeID);
    bool DispatchMapTypeMatchesMethodTable(DispatchMapTypeID typeID, MethodTable* pMT);

    // Determines whether all methods in the given interface have their final implementing
    // slot in a parent class. I.e. if this returns TRUE, it is trivial (no VSD lookup) to
    // dispatch pItfMT methods on this class if one knows how to dispatch them on pParentMT.
    BOOL ImplementsInterfaceWithSameSlotsAsParent(MethodTable *pItfMT, MethodTable *pParentMT);

    // Try to resolve a given static virtual method override on this type. Return nullptr
    // when not found.
    MethodDesc *TryResolveVirtualStaticMethodOnThisType(MethodTable* pInterfaceType, MethodDesc* pInterfaceMD, ResolveVirtualStaticMethodFlags resolveVirtualStaticMethodFlags, ClassLoadLevel level);

public:
    static MethodDesc *MapMethodDeclToMethodImpl(MethodDesc *pMDDecl);

    //-------------------------------------------------------------------
    // FINALIZATION SEMANTICS
    //

    DWORD  CannotUseSuperFastHelper()
    {
        WRAPPER_NO_CONTRACT;
        return HasFinalizer();
    }

    void SetHasFinalizer()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_HasFinalizer);
    }

    void SetHasCriticalFinalizer()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(!HasComponentSize());
        SetFlag(enum_flag_HasCriticalFinalizer);
    }
    // Does this class have non-trivial finalization requirements?
    DWORD HasFinalizer()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_HasFinalizer);
    }
    // Must this class be finalized during a rude appdomain unload, and
    // must it's finalizer run in a different order from normal finalizers?
    DWORD HasCriticalFinalizer() const
    {
        LIMITED_METHOD_CONTRACT;
        return !HasComponentSize() && GetFlag(enum_flag_HasCriticalFinalizer);
    }

    //-------------------------------------------------------------------
    // STATIC FIELDS
    //

    DWORD  GetOffsetOfFirstStaticHandle();
    DWORD  GetOffsetOfFirstStaticMT();

    inline PTR_BYTE GetNonGCStaticsBasePointer();
    inline PTR_BYTE GetGCStaticsBasePointer();
#ifndef DACCESS_COMPILE
    inline PTR_BYTE GetNonGCThreadStaticsBasePointer();
    inline PTR_BYTE GetGCThreadStaticsBasePointer();
#endif //!DACCESS_COMPILE

    // Do not use except in DAC and profiler scenarios.
    // These apis are difficult to use correctly. Users must
    // 1. Be aware that a GC may make the address returned invalid
    // 2. Be aware that a thread shutdown may make the address returned invalid
    // 3. Be aware that a collectible assembly could be collected, thus rendering the address returned invalid
    // This is particularly relevant as a problem for profiler developers, but they are given the tools (such as GC events) to be notified of situations where these invariants may not hold
    inline PTR_BYTE GetNonGCThreadStaticsBasePointer(PTR_Thread pThread);
    inline PTR_BYTE GetGCThreadStaticsBasePointer(PTR_Thread pThread);

    inline BOOL IsDynamicStatics()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_DynamicStatics) == enum_flag_DynamicStatics;
    }

    inline void SetDynamicStatics()
    {
        LIMITED_METHOD_CONTRACT;
        SetFlag(enum_flag_DynamicStatics);
    }

    inline void SetHasBoxedRegularStatics()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(!HasComponentSize());
        SetFlag(enum_flag_HasBoxedRegularStatics);
    }

    inline DWORD HasBoxedRegularStatics()
    {
        LIMITED_METHOD_CONTRACT;
        return GetFlag(enum_flag_HasBoxedRegularStatics);
    }

    inline void SetHasBoxedThreadStatics()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(!HasComponentSize());
        SetFlag(enum_flag_HasBoxedThreadStatics);
    }

    inline DWORD HasBoxedThreadStatics()
    {
        LIMITED_METHOD_CONTRACT;
        return GetFlag(enum_flag_HasBoxedThreadStatics);
    }

    DWORD HasFixedAddressVTStatics();

    // Indicates if the MethodTable only contains abstract methods
    BOOL HasOnlyAbstractMethods();

    //-------------------------------------------------------------------
    // PER-INSTANTIATION STATICS INFO
    //


    void SetupGenericsStaticsInfo(FieldDesc* pStaticFieldDescs);

    BOOL HasGenericsStaticsInfo()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return IsDynamicStatics() && HasInstantiation();
    }

    PTR_FieldDesc GetGenericsStaticFieldDescs()
    {
        WRAPPER_NO_CONTRACT;
        _ASSERTE(HasGenericsStaticsInfo());
        return GetGenericsStaticsInfo()->m_pFieldDescs;
    }

    WORD GetNumHandleRegularStatics();

    //-------------------------------------------------------------------
    // GENERICS DICT INFO
    //

    // Number of generic arguments, whether this is a method table for
    // a generic type instantiation, e.g. List<string> or the "generic" MethodTable
    // e.g. for List.
    inline DWORD GetNumGenericArgs()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        if (HasInstantiation())
            return (DWORD) (GetGenericsDictInfo()->m_wNumTyPars);
        else
            return 0;
    }

    inline DWORD GetNumDicts()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        if (HasPerInstInfo())
        {
            PTR_GenericsDictInfo  pDictInfo = GetGenericsDictInfo();
            return (DWORD) (pDictInfo->m_wNumDicts);
        }
        else
            return 0;
    }

    //-------------------------------------------------------------------
    // OBJECTS
    //

    OBJECTREF Allocate();

    // This flavor of Allocate is more efficient, but can only be used
    // if CheckInstanceActivated(), IsClassInited() are known to be true.
    // A sufficient condition is that another instance of the exact same type already
    // exists in the same ALC. It's currently called only from Delegate.Combine
    // via RuntimeTypeHandle_InternalAllocNoChecks.
    OBJECTREF AllocateNoChecks();

    OBJECTREF Box(void* data);
    OBJECTREF FastBox(void** data);
#ifndef DACCESS_COMPILE
    void UnBoxIntoUnchecked(void *dest, OBJECTREF src);
#endif

#ifdef _DEBUG
    // Used for debugging class layout. Dumps to the debug console
    // when debug is true.
    void DebugDumpVtable(LPCUTF8 szClassName, BOOL fDebug);
    void Debug_DumpInterfaceMap(LPCSTR szInterfaceMapPrefix);
    void Debug_DumpDispatchMap();
    void DebugDumpFieldLayout(LPCUTF8 pszClassName, BOOL debug);
    void DebugRecursivelyDumpInstanceFields(LPCUTF8 pszClassName, BOOL debug);
    void DebugDumpGCDesc(LPCUTF8 pszClassName, BOOL debug);
#endif //_DEBUG

    //-------------------------------------------------------------------
    // ENUMS, DELEGATES, VALUE TYPES, ARRAYS
    //
    // #KindsOfElementTypes
    // GetInternalCorElementType() retrieves the internal representation of the type. It's not always
    // appropriate to use this. For example, we treat enums as their underlying type or some structs are
    // optimized to be ints. To get the signature type or the verifier type (same as signature except for
    // enums are normalized to the primitive type that underlies them), use the APIs in Typehandle.h
    //
    //   * code:TypeHandle.GetSignatureCorElementType()
    //   * code:TypeHandle.GetVerifierCorElementType()
    //   * code:TypeHandle.GetInternalCorElementType()
    CorElementType GetInternalCorElementType();
    void SetInternalCorElementType(CorElementType _NormType);

    // See code:TypeHandle::GetVerifierCorElementType for description
    CorElementType GetVerifierCorElementType();

    // See code:TypeHandle::GetSignatureCorElementType for description
    CorElementType GetSignatureCorElementType();

    // A true primitive is one who's GetVerifierCorElementType() ==
    //      ELEMENT_TYPE_I,
    //      ELEMENT_TYPE_I4,
    //      ELEMENT_TYPE_TYPEDBYREF etc.
    // Note that GetIntenalCorElementType might return these same values for some additional
    // types such as Enums and some structs.
    BOOL IsTruePrimitive();
    void SetIsTruePrimitive();

    // Is this delegate? Returns false for System.Delegate and System.MulticastDelegate.
    inline BOOL IsDelegate()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        // We do not allow single cast delegates anymore, just check for multicast delegate
        _ASSERTE(g_pMulticastDelegateClass);
        return ParentEquals(g_pMulticastDelegateClass);
    }

    // Is this System.Object?
    inline BOOL IsObjectClass()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(g_pObjectClass);
        return (this == g_pObjectClass);
    }

    // Is this System.ValueType?
    inline DWORD IsValueTypeClass()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(g_pValueTypeClass);
        return (this == g_pValueTypeClass);
    }

    // Is this value type? Returns false for System.ValueType and System.Enum.
    inline BOOL IsValueType();

    // Is this enum? Returns false for System.Enum.
    inline BOOL IsEnum();

    // Is this array? Returns false for System.Array.
    inline BOOL IsArray()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_Category_Array_Mask) == enum_flag_Category_Array;
    }
    inline BOOL IsMultiDimArray()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        PRECONDITION(IsArray());
        return !GetFlag(enum_flag_Category_IfArrayThenSzArray);
    }

    // Returns true if this type is Nullable<T> for some T.
    inline BOOL IsNullable() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetFlag(enum_flag_Category_Mask) == enum_flag_Category_Nullable;
    }

    inline void SetIsNullable()
    {
        LIMITED_METHOD_CONTRACT;
        _ASSERTE(GetFlag(enum_flag_Category_Mask) == enum_flag_Category_ValueType);
        SetFlag(enum_flag_Category_Nullable);
    }

    // The following methods are only valid for the method tables for array types.
    CorElementType GetArrayElementType()
    {
        return GetArrayElementTypeHandle().GetSignatureCorElementType();
    }

    DWORD GetRank();

    TypeHandle GetArrayElementTypeHandle()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE (IsArray());
        return TypeHandle::FromTAddr(m_ElementTypeHnd);
    }

    void SetArrayElementTypeHandle(TypeHandle th)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        m_ElementTypeHnd = th.AsTAddr();
    }

    TypeHandle * GetArrayElementTypeHandlePtr()
    {
        LIMITED_METHOD_CONTRACT;
        return (TypeHandle *)&m_ElementTypeHnd;
    }

    static inline DWORD GetOffsetOfArrayElementTypeHandle()
    {
        LIMITED_METHOD_CONTRACT;
        return offsetof(MethodTable, m_ElementTypeHnd);
    }

    //-------------------------------------------------------------------
    // UNDERLYING METADATA
    //


    // Get the RID/token for the metadata for the corresponding type declaration
    unsigned GetTypeDefRid()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_dwFlags2 >> 8;
    }

    inline mdTypeDef GetCl()
    {
        LIMITED_METHOD_CONTRACT;
        return TokenFromRid(GetTypeDefRid(), mdtTypeDef);
    }

    void SetCl(mdTypeDef token);

    // Get the MD Import for the metadata for the corresponding type declaration
    IMDInternalImport* GetMDImport();

    HRESULT GetCustomAttribute(WellKnownAttribute attribute,
                               const void  **ppData,
                               ULONG *pcbData);

    mdTypeDef GetEnclosingCl();

    CorNativeLinkType GetCharSet();

#ifdef DACCESS_COMPILE
    void EnumMemoryRegions(CLRDataEnumMemoryFlags flags);
#endif

    //-------------------------------------------------------------------
    // DICTIONARIES FOR GENERIC INSTANTIATIONS
    //
    // The PerInstInfo pointer is a pointer to per-instantiation pointer table,
    // each entry of which points to an instantiation "dictionary"
    // for an instantiated type; the last pointer points to a
    // dictionary which is specific to this method table, previous
    // entries point to dictionaries in superclasses. Instantiated interfaces and structs
    // have just single dictionary (no inheritance).
    //
    // GetNumDicts() gives the number of dictionaries.
    //
    //@nice GENERICS: instead of a separate table of pointers, put the pointers
    // in the vtable itself. Advantages:
    // * Time: we save an indirection as we don't need to go through PerInstInfo first.
    // * Space: no need for PerInstInfo (1 word)
    // Problem is that lots of code assumes that the vtable is filled
    // uniformly with pointers to MethodDesc stubs.
    //
    // The dictionary for the method table is just an array of handles for
    // type parameters in the following cases:
    // * instantiated interfaces (no code)
    // * instantiated types whose code is not shared
    // Otherwise, it starts with the type parameters and then has a fixed
    // number of slots for handles (types & methods)
    // that are filled in lazily at run-time. Finally there is a "spill-bucket"
    // pointer used when the dictionary gets filled.
    // In summary:
    //    typar_1              type handle for first type parameter
    //    ...
    //    typar_n              type handle for last type parameter
    //    slot_1               slot for first run-time handle (initially null)
    //    ...
    //    slot_m               slot for last run-time handle (initially null)
    //    next_bucket          pointer to spill bucket (possibly null)
    // The spill bucket contains just run-time handle slots.
    //   (Alternative: continue chaining buckets.
    //    Advantage: no need to deallocate when growing dictionaries.
    //    Disadvantage: more indirections required at run-time.)
    //
    // The layout of dictionaries is determined by GetClass()->GetDictionaryLayout()
    // Thus the layout can vary between incompatible instantiations. This is sometimes useful because individual type
    // parameters may or may not be shared. For example, consider a two parameter class Dict<K,D>. In instantiations shared with
    // Dict<double,string> any reference to K is known at JIT-compile-time (it's double) but any token containing D
    // must have a dictionary entry. On the other hand, for instantiations shared with Dict<string,double> the opposite holds.
    //

    typedef PTR_Dictionary PerInstInfoElem_t;
    typedef DPTR(PerInstInfoElem_t) PerInstInfo_t;

    // Return a pointer to the per-instantiation information. See field itself for comments.
    DPTR(PerInstInfoElem_t) GetPerInstInfo()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(HasPerInstInfo());
        return m_pPerInstInfo;
    }
    BOOL HasPerInstInfo()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        // Assert that either this is an array, or m_pPerInstInfo is non-NULL only if HasPerInstInfo is set
        _ASSERTE(IsArray() || (m_pPerInstInfo == NULL) == !GetFlag(enum_flag_HasPerInstInfo));

        // Assert that this if this is an Array, HasPerInstInfo is not set
        _ASSERTE(!IsArray() || !GetFlag(enum_flag_HasPerInstInfo));
        return GetFlag(enum_flag_HasPerInstInfo);
    }
#ifndef DACCESS_COMPILE
    static inline DWORD GetOffsetOfPerInstInfo()
    {
        LIMITED_METHOD_CONTRACT;
        return offsetof(MethodTable, m_pPerInstInfo);
    }
    void SetPerInstInfo(PerInstInfoElem_t *pPerInstInfo)
    {
        LIMITED_METHOD_CONTRACT;
        m_pPerInstInfo = pPerInstInfo;
    }
    void SetDictInfo(WORD numDicts, WORD numTyPars)
    {
        WRAPPER_NO_CONTRACT;
        GenericsDictInfo* pInfo = GetGenericsDictInfo();
        pInfo->m_wNumDicts  = numDicts;
        pInfo->m_wNumTyPars = numTyPars;
    }
#endif // !DACCESS_COMPILE
    PTR_GenericsDictInfo GetGenericsDictInfo()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        // GenericsDictInfo is stored at negative offset of the dictionary
        return dac_cast<PTR_GenericsDictInfo>(GetPerInstInfo()) - 1;
    }

    // Get a pointer to the dictionary for this instantiated type
    // (The instantiation is stored in the initial slots of the dictionary)
    // If not instantiated, return NULL
    PTR_Dictionary GetDictionary();

    // Return a substitution suitbale for interpreting
    // the metadata in parent class, assuming we already have a subst.
    // suitable for interpreting the current class.
    //
    // If, for example, the definition for the current class is
    //   D<T> : C<List<T>, T[] >
    // then this (for C<!0,!1>) will be
    //   0 --> List<T>
    //   1 --> T[]
    // added to the chain of substitutions.
    //
    // Subsequently, if the definition for C is
    //   C<T, U> : B< Dictionary<T, U> >
    // then the next subst (for B<!0>) will be
    //   0 --> Dictionary< List<T>, T[] >

    Substitution GetSubstitutionForParent(const Substitution *pSubst);

    inline DWORD GetAttrClass();

    inline BOOL HasFieldsWhichMustBeInited();

    //-------------------------------------------------------------------
    // THE EXPOSED CLASS OBJECT
    //
    /*
     * m_ExposedClassObject is a RuntimeType instance for this class.  But
     * do NOT use it for Arrays or remoted objects!  All arrays of objects
     * share the same MethodTable/EEClass.
     * @GENERICS: this is per-instantiation data
     */
    // There are two version of GetManagedClassObject.  The GetManagedClassObject()
    //  method will get the class object.  If it doesn't exist it will be created.
    //  GetManagedClassObjectIfExists() will return null if the Type object doesn't exist.
    OBJECTREF GetManagedClassObject();
    OBJECTREF GetManagedClassObjectIfExists();

    // ------------------------------------------------------------------
    // Details about Nullable<T> MethodTables
    // ------------------------------------------------------------------
    UINT32 GetNullableValueAddrOffset() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(IsNullable());
#ifndef TARGET_64BIT
        return *(BYTE*)&m_encodedNullableUnboxData;
#else
        return *(UINT32*)&m_encodedNullableUnboxData;
#endif
    }

    UINT32 GetNullableValueSize() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(IsNullable());
#ifndef TARGET_64BIT
        return (UINT32)(m_encodedNullableUnboxData >> 8);
#else
        return (UINT32)(m_encodedNullableUnboxData >> 32);
#endif
    }

    UINT32 GetNullableNumInstanceFieldBytes() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        _ASSERTE(IsNullable());
        return GetNullableValueAddrOffset() + GetNullableValueSize();
    }

    // ------------------------------------------------------------------
    // Private part of MethodTable
    // ------------------------------------------------------------------

#ifndef DACCESS_COMPILE
    void AllocateAuxiliaryData(LoaderAllocator *pAllocator, Module *pLoaderModule, AllocMemTracker *pamTracker, MethodTableStaticsFlags staticsFlags = MethodTableStaticsFlags::None, WORD nonVirtualSlots = 0, S_SIZE_T extraAllocation = S_SIZE_T(0));
#endif

    inline PTR_Const_MethodTableAuxiliaryData GetAuxiliaryData() const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return MethodTable::m_pAuxiliaryData;
    }

    inline PTR_MethodTableAuxiliaryData GetAuxiliaryDataForWrite()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return MethodTable::m_pAuxiliaryData;
    }

    DWORD* getIsClassInitedFlagAddress()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetAuxiliaryDataForWrite()->getIsClassInitedFlagAddress();
    }

    //-------------------------------------------------------------------
    // The GUID Info
    // Used by COM interop to get GUIDs (IIDs and CLSIDs)

    // Get and cache the GUID for this interface/class
    HRESULT GetGuidNoThrow(GUID *pGuid, BOOL bGenerateIfNotFound, BOOL bClassic = TRUE);

    // Get and cache the GUID for this interface/class
    void    GetGuid(GUID *pGuid, BOOL bGenerateIfNotFound, BOOL bClassic = TRUE);

    // Convenience method - determine if the interface/class has a guid specified (even if not yet cached)
    BOOL HasExplicitGuid();

public :
    // Helper routines for the GetFullyQualifiedNameForClass macros defined at the top of class.h.
    // You probably should not use these functions directly.
    SString &_GetFullyQualifiedNameForClassNestedAware(SString &ssBuf);
    SString &_GetFullyQualifiedNameForClass(SString &ssBuf);
    LPCUTF8 GetFullyQualifiedNameInfo(LPCUTF8 *ppszNamespace);

public :
    //-------------------------------------------------------------------
    // Debug Info
    //


#ifdef _DEBUG
    inline LPCUTF8 GetDebugClassName()
    {
        LIMITED_METHOD_CONTRACT;
        return debug_m_szClassName;
    }
    inline void SetDebugClassName(LPCUTF8 name)
    {
        LIMITED_METHOD_CONTRACT;
        debug_m_szClassName = name;
    }

    // Was the type created with injected duplicates?
    // TRUE means that we tried to inject duplicates (not that we found one to inject).
    inline BOOL Debug_HasInjectedInterfaceDuplicates() const
    {
        LIMITED_METHOD_CONTRACT;
        return (GetAuxiliaryData()->m_dwFlagsDebug & MethodTableAuxiliaryData::enum_flagDebug_HasInjectedInterfaceDuplicates) != 0;
    }
    inline void Debug_SetHasInjectedInterfaceDuplicates()
    {
        LIMITED_METHOD_CONTRACT;
        GetAuxiliaryDataForWrite()->m_dwFlagsDebug |= MethodTableAuxiliaryData::enum_flagDebug_HasInjectedInterfaceDuplicates;
    }
#endif // _DEBUG


#ifndef DACCESS_COMPILE
public:
    //--------------------------------------------------------------------------------------
    class MethodData
    {
      public:
        inline ULONG AddRef()
            { LIMITED_METHOD_CONTRACT; return (ULONG) InterlockedIncrement((LONG*)&m_cRef); }

        ULONG Release();

        // Since all methods that return a MethodData already AddRef'd, we do NOT
        // want to AddRef when putting a holder around it. We only want to release it.
        static void HolderAcquire(MethodData *pEntry)
            { LIMITED_METHOD_CONTRACT; return; }
        static void HolderRelease(MethodData *pEntry)
            { WRAPPER_NO_CONTRACT; if (pEntry != NULL) pEntry->Release(); }

      protected:
        ULONG m_cRef;
        MethodTable *const m_pImplMT;
        MethodTable *const m_pDeclMT;

      public:
        MethodData(MethodTable *implMT, MethodTable *declMT) : m_cRef(1), m_pImplMT(implMT), m_pDeclMT(declMT) { LIMITED_METHOD_CONTRACT; }
        virtual ~MethodData() { LIMITED_METHOD_CONTRACT; }

        virtual MethodData  *GetDeclMethodData() = 0;
        MethodTable *GetDeclMethodTable() { return m_pDeclMT; }
        virtual MethodDesc  *GetDeclMethodDesc(UINT32 slotNumber) = 0;

        virtual MethodData  *GetImplMethodData() = 0;
        MethodTable *GetImplMethodTable() { return m_pImplMT; }
        virtual DispatchSlot GetImplSlot(UINT32 slotNumber) = 0;
        virtual bool IsImplSlotNull(UINT32 slotNumber) = 0;
        // Returns INVALID_SLOT_NUMBER if no implementation exists.
        virtual UINT32       GetImplSlotNumber(UINT32 slotNumber) = 0;
        virtual MethodDesc  *GetImplMethodDesc(UINT32 slotNumber) = 0;
        virtual void InvalidateCachedVirtualSlot(UINT32 slotNumber) = 0;

        virtual UINT32 GetNumVirtuals() = 0;
        virtual UINT32 GetNumMethods() = 0;

        virtual void UpdateImplMethodDesc(MethodDesc* pMD, UINT32 slotNumber) = 0;

      protected:
        static const UINT32 INVALID_SLOT_NUMBER = UINT32_MAX;

        // This is used when building the data
        struct MethodDataEntry
        {
          private:
            static const UINT32 INVALID_CHAIN_AND_INDEX = (UINT32)(-1);
            static const UINT16 INVALID_IMPL_SLOT_NUM = (UINT16)(-1);

            // This contains both the chain delta and the table index. The
            // reason that they are combined is that we need atomic update
            // of both, and it is convenient that both are on UINT16 in size.
            UINT32           m_chainDeltaAndTableIndex;
            UINT16           m_implSlotNum;     // For virtually remapped slots
            DispatchSlot     m_slot;            // The entry in the DispatchImplTable
            MethodDesc      *m_pMD;             // The MethodDesc for this slot

          public:
            inline MethodDataEntry() : m_slot((PCODE)NULL)
                { WRAPPER_NO_CONTRACT; Init(); }

            inline void Init()
            {
                LIMITED_METHOD_CONTRACT;
                m_chainDeltaAndTableIndex = INVALID_CHAIN_AND_INDEX;
                m_implSlotNum = INVALID_IMPL_SLOT_NUM;
                m_slot = (PCODE)NULL;
                m_pMD = NULL;
            }

            inline BOOL IsDeclInit()
                { LIMITED_METHOD_CONTRACT; return m_chainDeltaAndTableIndex != INVALID_CHAIN_AND_INDEX; }
            inline BOOL IsImplInit()
                { LIMITED_METHOD_CONTRACT; return m_implSlotNum != INVALID_IMPL_SLOT_NUM; }

            inline void SetDeclData(UINT32 chainDelta, UINT32 tableIndex)
                { LIMITED_METHOD_CONTRACT; m_chainDeltaAndTableIndex = ((((UINT16) chainDelta) << 16) | ((UINT16) tableIndex)); }
            inline UINT32 GetChainDelta()
                { LIMITED_METHOD_CONTRACT; CONSISTENCY_CHECK(IsDeclInit()); return m_chainDeltaAndTableIndex >> 16; }
            inline UINT32 GetTableIndex()
                { LIMITED_METHOD_CONTRACT; CONSISTENCY_CHECK(IsDeclInit()); return (m_chainDeltaAndTableIndex & (UINT32)UINT16_MAX); }

            inline void SetImplData(UINT32 implSlotNum)
                { LIMITED_METHOD_CONTRACT; m_implSlotNum = (UINT16) implSlotNum; }
            inline UINT32 GetImplSlotNum()
                { LIMITED_METHOD_CONTRACT; CONSISTENCY_CHECK(IsImplInit()); return m_implSlotNum; }

            inline void SetSlot(DispatchSlot slot)
                { LIMITED_METHOD_CONTRACT; m_slot = slot; }
            inline DispatchSlot GetSlot()
                { LIMITED_METHOD_CONTRACT; return m_slot; }

            inline void SetMethodDesc(MethodDesc *pMD)
                { LIMITED_METHOD_CONTRACT; m_pMD = pMD; }
            inline MethodDesc *GetMethodDesc()
                { LIMITED_METHOD_CONTRACT; return m_pMD; }

        };

        static void ProcessMap(
            const DispatchMapTypeID * rgTypeIDs,
            UINT32                    cTypeIDs,
            MethodTable *             pMT,
            UINT32                    cCurrentChainDepth,
            MethodDataEntry *         rgWorkingData,
            size_t                    cWorkingData);
    };  // class MethodData

    typedef ::Holder < MethodData *, MethodData::HolderAcquire, MethodData::HolderRelease > MethodDataHolder;
    typedef ::Wrapper < MethodData *, MethodData::HolderAcquire, MethodData::HolderRelease > MethodDataWrapper;

protected:
    //--------------------------------------------------------------------------------------
    class MethodDataObject final : public MethodData
    {
      public:
        // Static method that returns the amount of memory to allocate for a particular type.
        static UINT32 GetObjectSize(MethodTable *pMT, MethodDataComputeOptions computeOptions);

        // Constructor. Make sure you have allocated enough memory using GetObjectSize.
        inline MethodDataObject(MethodTable *pMT, MethodDataComputeOptions computeOptions) :
            MethodData(pMT, pMT),
            m_numMethods(ComputeNumMethods(pMT, computeOptions)),
            m_virtualsOnly(computeOptions == MethodDataComputeOptions::NoCacheVirtualsOnly)
            { WRAPPER_NO_CONTRACT; _ASSERTE(computeOptions != MethodDataComputeOptions::CacheOnly); Init(NULL); }

        inline MethodDataObject(MethodTable *pMT, MethodData *pParentData, MethodDataComputeOptions computeOptions) :
            MethodData(pMT, pMT),
            m_numMethods(ComputeNumMethods(pMT, computeOptions)),
            m_virtualsOnly(computeOptions == MethodDataComputeOptions::NoCacheVirtualsOnly)
            { WRAPPER_NO_CONTRACT; _ASSERTE(computeOptions != MethodDataComputeOptions::CacheOnly); Init(pParentData); }

        virtual ~MethodDataObject() { LIMITED_METHOD_CONTRACT; }

        virtual MethodData  *GetDeclMethodData()
            { LIMITED_METHOD_CONTRACT; return this; }
        virtual MethodDesc *GetDeclMethodDesc(UINT32 slotNumber);

        virtual MethodData  *GetImplMethodData()
            { LIMITED_METHOD_CONTRACT; return this; }
        virtual DispatchSlot GetImplSlot(UINT32 slotNumber);
        virtual bool IsImplSlotNull(UINT32 slotNumber) { LIMITED_METHOD_CONTRACT; return false; } // Every valid slot on an actual MethodTable has a MethodDesc which is associated with it
        virtual UINT32       GetImplSlotNumber(UINT32 slotNumber);
        virtual MethodDesc  *GetImplMethodDesc(UINT32 slotNumber);
        virtual void InvalidateCachedVirtualSlot(UINT32 slotNumber);

        virtual UINT32 GetNumVirtuals()
            { LIMITED_METHOD_CONTRACT; return m_pDeclMT->GetNumVirtuals(); }

        static UINT32 ComputeNumMethods(MethodTable *pMT, MethodDataComputeOptions computeOptions)
        {
            LIMITED_METHOD_DAC_CONTRACT;

            // MethodDataComputeOptions::CacheOnly is used for asking for a MethodDataObject that already exists,
            // so there should never be a reason to ask what the size of a new one might be.
            _ASSERTE(computeOptions != MethodDataComputeOptions::CacheOnly);

            if (computeOptions == MethodDataComputeOptions::NoCacheVirtualsOnly)
            {
                return pMT->GetCanonicalMethodTable()->GetNumVtableSlots();
            }
            else
            {
                return pMT->GetCanonicalMethodTable()->GetNumMethods();
            }
        }

        virtual UINT32 GetNumMethods()
            { LIMITED_METHOD_CONTRACT; return m_numMethods; }

        virtual void UpdateImplMethodDesc(MethodDesc* pMD, UINT32 slotNumber);

      protected:
        void Init(MethodData *pParentData);

        BOOL PopulateNextLevel();

        // This is used in staged map decoding - it indicates which type we will next decode.
        UINT32       m_iNextChainDepth;
        static const UINT32 MAX_CHAIN_DEPTH = UINT32_MAX;

        UINT32       m_numMethods;
        bool         m_virtualsOnly;
        bool         m_containsMethodImpl;

        // NOTE: Use of these APIs are unlocked and may appear to be erroneous. However, since calls
        //       to ProcessMap will result in identical values being placed in the MethodDataObjectEntry
        //       array, it it is not a problem if there is a race, since one thread may just end up
        //       doing some duplicate work.

        inline UINT32 GetNextChainDepth()
        { LIMITED_METHOD_CONTRACT; return VolatileLoad(&m_iNextChainDepth); }

        inline void SetNextChainDepth(UINT32 iDepth)
        {
            LIMITED_METHOD_CONTRACT;
            if (GetNextChainDepth() < iDepth) {
                VolatileStore(&m_iNextChainDepth, iDepth);
            }
        }

        // This is used when building the data
        struct MethodDataObjectEntry
        {
          private:
            MethodDesc *m_pMDDecl;
            MethodDesc *m_pMDImpl;

          public:
            inline MethodDataObjectEntry() : m_pMDDecl(NULL), m_pMDImpl(NULL) {}

            inline void SetDeclMethodDesc(MethodDesc *pMD)
                { LIMITED_METHOD_CONTRACT; m_pMDDecl = pMD; }
            inline MethodDesc *GetDeclMethodDesc()
                { LIMITED_METHOD_CONTRACT; return m_pMDDecl; }
            inline void SetImplMethodDesc(MethodDesc *pMD)
                { LIMITED_METHOD_CONTRACT; m_pMDImpl = pMD; }
            inline MethodDesc *GetImplMethodDesc()
                { LIMITED_METHOD_CONTRACT; return m_pMDImpl; }
        };


        inline MethodDataObjectEntry *GetEntryData()
            { LIMITED_METHOD_CONTRACT; return &m_rgEntries[0]; }

        inline MethodDataObjectEntry *GetEntry(UINT32 i)
            { LIMITED_METHOD_CONTRACT; CONSISTENCY_CHECK(i < GetNumMethods()); return GetEntryData() + i; }

        void FillEntryDataForAncestor(MethodTable *pMT);

        //
        // At the end of this object is an array
        //
        MethodDataObjectEntry m_rgEntries[0];

      public:
        struct TargetMethodTable
        {
            MethodTable* pMT;
        };

        static void* operator new(size_t size, TargetMethodTable targetMT, MethodDataComputeOptions computeOptions)
        {
            _ASSERTE(computeOptions != MethodDataComputeOptions::CacheOnly);
            _ASSERTE(size <= GetObjectSize(targetMT.pMT, computeOptions));
            return ::operator new(GetObjectSize(targetMT.pMT, computeOptions));
        }
        static void* operator new(size_t size) = delete;
    };  // class MethodDataObject

    //--------------------------------------------------------------------------------------
    class MethodDataInterface : public MethodData
    {
      public:
        // Static method that returns the amount of memory to allocate for a particular type.
        static UINT32 GetObjectSize(MethodTable *pMT)
            { LIMITED_METHOD_CONTRACT; return sizeof(MethodDataInterface); }

        // Constructor. Make sure you have allocated enough memory using GetObjectSize.
        MethodDataInterface(MethodTable *pMT) : MethodData(pMT, pMT)
        {
            LIMITED_METHOD_CONTRACT;
            CONSISTENCY_CHECK(CheckPointer(pMT));
            CONSISTENCY_CHECK(pMT->IsInterface());
        }
        virtual ~MethodDataInterface()
            { LIMITED_METHOD_CONTRACT; }

        //
        // Decl data
        //
        virtual MethodData  *GetDeclMethodData()
            { LIMITED_METHOD_CONTRACT; return this; }
        virtual MethodDesc *GetDeclMethodDesc(UINT32 slotNumber);

        //
        // Impl data
        //
        virtual MethodData  *GetImplMethodData()
            { LIMITED_METHOD_CONTRACT; return this; }
        virtual DispatchSlot GetImplSlot(UINT32 slotNumber)
            { WRAPPER_NO_CONTRACT; return DispatchSlot(m_pDeclMT->GetRestoredSlot(slotNumber)); }
        virtual bool IsImplSlotNull(UINT32 slotNumber)
        {
            // Every valid slot on an actual MethodTable has a MethodDesc which is associated with it
            LIMITED_METHOD_CONTRACT;
            return false;
        }
        virtual UINT32       GetImplSlotNumber(UINT32 slotNumber)
            { LIMITED_METHOD_CONTRACT; return slotNumber; }
        virtual MethodDesc  *GetImplMethodDesc(UINT32 slotNumber);
        virtual void InvalidateCachedVirtualSlot(UINT32 slotNumber);

        //
        // Slot count data
        //
        virtual UINT32 GetNumVirtuals()
            { LIMITED_METHOD_CONTRACT; return m_pDeclMT->GetNumVirtuals(); }
        virtual UINT32 GetNumMethods()
            { LIMITED_METHOD_CONTRACT; return m_pDeclMT->GetNumMethods(); }

        virtual void UpdateImplMethodDesc(MethodDesc* pMD, UINT32 slotNumber)
            { LIMITED_METHOD_CONTRACT; }

    };  // class MethodDataInterface

    //--------------------------------------------------------------------------------------
    class MethodDataInterfaceImpl final : public MethodData
    {
      public:
        // Object construction-related methods
        static UINT32 GetObjectSize(MethodTable *pMTDecl);

        MethodDataInterfaceImpl(
            const DispatchMapTypeID * rgDeclTypeIDs,
            UINT32                    cDeclTypeIDs,
            MethodData *              pDecl,
            MethodData *              pImpl);
        virtual ~MethodDataInterfaceImpl();

        // Decl-related methods
        virtual MethodData  *GetDeclMethodData()
            { LIMITED_METHOD_CONTRACT; return m_pDecl; }
        virtual MethodTable *GetDeclMethodTable()
            { WRAPPER_NO_CONTRACT; return m_pDecl->GetDeclMethodTable(); }
        virtual MethodDesc  *GetDeclMethodDesc(UINT32 slotNumber)
            { WRAPPER_NO_CONTRACT; return m_pDecl->GetDeclMethodDesc(slotNumber); }

        // Impl-related methods
        virtual MethodData  *GetImplMethodData()
            { LIMITED_METHOD_CONTRACT; return m_pImpl; }
        virtual MethodTable *GetImplMethodTable()
            { WRAPPER_NO_CONTRACT; return m_pImpl->GetImplMethodTable(); }
        virtual DispatchSlot GetImplSlot(UINT32 slotNumber);
        virtual bool IsImplSlotNull(UINT32 slotNumber);
        virtual UINT32       GetImplSlotNumber(UINT32 slotNumber);
        virtual MethodDesc  *GetImplMethodDesc(UINT32 slotNumber);
        virtual void InvalidateCachedVirtualSlot(UINT32 slotNumber);

        virtual UINT32 GetNumVirtuals()
            { WRAPPER_NO_CONTRACT; return m_pDecl->GetNumVirtuals(); }
        virtual UINT32 GetNumMethods()
            { WRAPPER_NO_CONTRACT; return m_pDecl->GetNumVirtuals(); }

        virtual void UpdateImplMethodDesc(MethodDesc* pMD, UINT32 slotNumber)
            { LIMITED_METHOD_CONTRACT; }

      protected:
        UINT32 MapToImplSlotNumber(UINT32 slotNumber);

        BOOL PopulateNextLevel();
        void Init(
            const DispatchMapTypeID * rgDeclTypeIDs,
            UINT32                    cDeclTypeIDs,
            MethodData *              pDecl,
            MethodData *              pImpl);

        MethodData *m_pDecl;
        MethodData *m_pImpl;

        // This is used in staged map decoding - it indicates which type(s) we will find.
        const DispatchMapTypeID * m_rgDeclTypeIDs;
        UINT32                    m_cDeclTypeIDs;
        UINT32                    m_iNextChainDepth;
        static const UINT32       MAX_CHAIN_DEPTH = UINT32_MAX;

        inline UINT32 GetNextChainDepth()
        { LIMITED_METHOD_CONTRACT; return VolatileLoad(&m_iNextChainDepth); }

        inline void SetNextChainDepth(UINT32 iDepth)
        {
            LIMITED_METHOD_CONTRACT;
            if (GetNextChainDepth() < iDepth) {
                VolatileStore(&m_iNextChainDepth, iDepth);
            }
        }

        //
        // At the end of this object is an array, so you cannot derive from this class.
        //

        inline MethodDataEntry *GetEntryData()
            { LIMITED_METHOD_CONTRACT; return &m_rgEntries[0]; }

        inline MethodDataEntry *GetEntry(UINT32 i)
            { LIMITED_METHOD_CONTRACT; CONSISTENCY_CHECK(i < GetNumMethods()); return GetEntryData() + i; }

        MethodDataEntry m_rgEntries[0];

      public:
        struct TargetMethodTable
        {
            MethodTable* pMT;
        };

        static void* operator new(size_t size, TargetMethodTable targetMT)
        {
            _ASSERTE(size <= GetObjectSize(targetMT.pMT));
            return ::operator new(GetObjectSize(targetMT.pMT));
        }
        static void* operator new(size_t size) = delete;
    };  // class MethodDataInterfaceImpl

    //--------------------------------------------------------------------------------------
    static MethodDataCache *s_pMethodDataCache;

public:
    static void InitMethodDataCache();
    static void ClearMethodDataCache();
    // NOTE: The computeOption argument determines if the resulting MethodData object can
    //       be added to the global MethodDataCache. This is used when requesting a
    //       MethodData object for a type currently being built.
    static MethodData *GetMethodData(MethodTable *pMT, MethodDataComputeOptions computeOption);
    static MethodData *GetMethodData(MethodTable *pMTDecl, MethodTable *pMTImpl, MethodDataComputeOptions computeOption);
    // This method is used by BuildMethodTable because the exact interface has not yet been loaded.
    // NOTE: This method does not cache the resulting MethodData object in the global MethodDataCache.
    static MethodData * GetMethodData(
        const DispatchMapTypeID * rgDeclTypeIDs,
        UINT32                    cDeclTypeIDs,
        MethodTable *             pMTDecl,
        MethodTable *             pMTImpl,
        MethodDataComputeOptions computeOption);

    void CopySlotFrom(UINT32 slotNumber, MethodDataWrapper &hSourceMTData, MethodTable *pSourceMT);

protected:
    static MethodData *FindParentMethodDataHelper(MethodTable *pMT);
    static MethodData *FindMethodDataHelper(MethodTable *pMTDecl, MethodTable *pMTImpl);
    static MethodData *GetMethodDataHelper(MethodTable *pMTDecl, MethodTable *pMTImpl, MethodDataComputeOptions computeOption);
    // NOTE: This method does not cache the resulting MethodData object in the global MethodDataCache.
    static MethodData * GetMethodDataHelper(
        const DispatchMapTypeID * rgDeclTypeIDs,
        UINT32                    cDeclTypeIDs,
        MethodTable *             pMTDecl,
        MethodTable *             pMTImpl,
        MethodDataComputeOptions computeOption);

public:
    //--------------------------------------------------------------------------------------
    class MethodIterator
    {
    public:
        MethodIterator(MethodTable *pMT);
        MethodIterator(MethodTable *pMTDecl, MethodTable *pMTImpl);
        MethodIterator(MethodData *pMethodData);
        MethodIterator(const MethodIterator &it);
        inline ~MethodIterator() { WRAPPER_NO_CONTRACT; m_pMethodData->Release(); }
        INT32 GetNumMethods() const;
        inline BOOL IsValid() const;
        inline BOOL MoveTo(UINT32 idx);
        inline BOOL Prev();
        inline BOOL Next();
        inline void MoveToBegin();
        inline void MoveToEnd();
        inline UINT32 GetSlotNumber() const;
        inline UINT32 GetImplSlotNumber() const;
        inline BOOL IsVirtual() const;
        inline UINT32 GetNumVirtuals() const;
        inline DispatchSlot GetTarget() const;
        inline bool IsTargetNull() const;

        // Can be called only if IsValid()=TRUE
        inline MethodDesc *GetMethodDesc() const;
        inline MethodDesc *GetDeclMethodDesc() const;

    protected:
        void Init(MethodTable *pMTDecl, MethodTable *pMTImpl);

        MethodData         *m_pMethodData;
        INT32               m_iCur;           // Current logical slot index
        INT32               m_iMethods;
    };  // class MethodIterator
#endif // !DACCESS_COMPILE

    //--------------------------------------------------------------------------------------
    // This iterator lets you walk over all the method bodies introduced by this type.
    // This includes new static methods, new non-virtual methods, and any overrides
    // of the parent's virtual methods. It does not include virtual method implementations
    // provided by the parent

    class IntroducedMethodIterator
    {
    public:
        IntroducedMethodIterator(MethodTable *pMT, BOOL restrictToCanonicalTypes = TRUE);
        inline BOOL IsValid() const;
        BOOL Next();

        // Can be called only if IsValid()=TRUE
        inline MethodDesc *GetMethodDesc() const;

        // Static worker methods of the iterator. These are meant to be used
        // by RuntimeTypeHandle::GetFirstIntroducedMethod and RuntimeTypeHandle::GetNextIntroducedMethod
        // only to expose this iterator to managed code.
        static MethodDesc * GetFirst(MethodTable * pMT);
        static MethodDesc * GetNext(MethodDesc * pMD);

    protected:
        MethodDesc      *m_pMethodDesc;     // Current method desc

        // Cached info about current method desc
        MethodDescChunk *m_pChunk;
        TADDR            m_pChunkEnd;

        void SetChunk(MethodDescChunk * pChunk);
    };  // class IntroducedMethodIterator

    //-------------------------------------------------------------------
    // INSTANCE MEMBER VARIABLES
    //

#ifdef DACCESS_COMPILE
public:
#else
private:
#endif
    enum WFLAGS_LOW_ENUM
    {
        // AS YOU ADD NEW FLAGS PLEASE CONSIDER WHETHER Generics::NewInstantiation NEEDS
        // TO BE UPDATED IN ORDER TO ENSURE THAT METHODTABLES DUPLICATED FOR GENERIC INSTANTIATIONS
        // CARRY THE CORECT FLAGS.
        //

        // We are overloading the low 2 bytes of m_dwFlags to be a component size for Strings
        // and Arrays and some set of flags which we can be assured are of a specified state
        // for Strings / Arrays, currently these will be a bunch of generics flags which don't
        // apply to Strings / Arrays.

        enum_flag_UNUSED_ComponentSize_1    = 0x00000001,
        // GC depends on this bit
        enum_flag_HasCriticalFinalizer      = 0x00000002, // finalizer must be run on Appdomain Unload


        enum_flag_GenericsMask              = 0x00000030,
        enum_flag_GenericsMask_NonGeneric   = 0x00000000,   // no instantiation
        enum_flag_GenericsMask_GenericInst  = 0x00000010,   // regular instantiation, e.g. List<String>
        enum_flag_GenericsMask_SharedInst   = 0x00000020,   // shared instantiation, e.g. List<__Canon> or List<MyValueType<__Canon>>
        enum_flag_GenericsMask_TypicalInst  = 0x00000030,   // the type instantiated at its formal parameters, e.g. List<T>

        enum_flag_HasVariance               = 0x00000100,   // This is an instantiated type some of whose type parameters are co- or contra-variant

        enum_flag_HasDefaultCtor            = 0x00000200,
        enum_flag_HasPreciseInitCctors      = 0x00000400,   // Do we need to run class constructors at allocation time? (Not perf important, could be moved to EEClass

#if defined(FEATURE_HFA)
#if defined(UNIX_AMD64_ABI)
#error "Can't define both FEATURE_HFA and UNIX_AMD64_ABI"
#endif
        enum_flag_IsHFA                     = 0x00000800,   // This type is an HFA (Homogeneous Floating-point Aggregate)
#endif // FEATURE_HFA

#if defined(UNIX_AMD64_ABI)
#if defined(FEATURE_HFA)
#error "Can't define both FEATURE_HFA and UNIX_AMD64_ABI"
#endif
        enum_flag_IsRegStructPassed         = 0x00000800,   // This type is a System V register passed struct.
#endif // UNIX_AMD64_ABI

        enum_flag_IsByRefLike               = 0x00001000,

        enum_flag_HasBoxedRegularStatics    = 0x00002000,
        enum_flag_HasBoxedThreadStatics     = 0x00004000,

        // In a perfect world we would fill these flags using other flags that we already have
        // which have a constant value for something which has a component size.
        enum_flag_UNUSED_ComponentSize_7    = 0x00008000,

#define SET_FALSE(flag)     ((flag) & 0)
#define SET_TRUE(flag)      ((flag) & 0xffff)

        // IMPORTANT! IMPORTANT! IMPORTANT!
        //
        // As you change the flags in WFLAGS_LOW_ENUM you also need to change this
        // to be up to date to reflect the default values of those flags for the
        // case where this MethodTable is for a String or Array
        enum_flag_StringArrayValues = SET_FALSE(enum_flag_HasCriticalFinalizer) |
                                      SET_FALSE(enum_flag_HasBoxedRegularStatics) |
                                      SET_FALSE(enum_flag_HasBoxedThreadStatics) |
                                      SET_TRUE(enum_flag_GenericsMask_NonGeneric) |
                                      SET_FALSE(enum_flag_HasVariance) |
                                      SET_FALSE(enum_flag_HasDefaultCtor) |
                                      SET_FALSE(enum_flag_HasPreciseInitCctors),

    };  // enum WFLAGS_LOW_ENUM

    enum WFLAGS_HIGH_ENUM
    {
        // DO NOT use flags that have bits set in the low 2 bytes.
        // These flags are DWORD sized so that our atomic masking
        // operations can operate on the entire 4-byte aligned DWORD
        // instead of the logical non-aligned WORD of flags.  The
        // low WORD of flags is reserved for the component size.

        // The following bits describe mutually exclusive locations of the type
        // in the type hierarchy.
        enum_flag_Category_Mask             = 0x000F0000,

        enum_flag_Category_Class            = 0x00000000,
        enum_flag_Category_Unused_1         = 0x00010000,
        enum_flag_Category_Unused_2         = 0x00020000,
        enum_flag_Category_Unused_3         = 0x00030000,

        enum_flag_Category_ValueType        = 0x00040000,
        enum_flag_Category_ValueType_Mask   = 0x000C0000,
        enum_flag_Category_Nullable         = 0x00050000, // sub-category of ValueType
        enum_flag_Category_PrimitiveValueType=0x00060000, // sub-category of ValueType, Enum or primitive value type
        enum_flag_Category_TruePrimitive    = 0x00070000, // sub-category of ValueType, Primitive (ELEMENT_TYPE_I, etc.)

        enum_flag_Category_Array            = 0x00080000,
        enum_flag_Category_Array_Mask       = 0x000C0000,
        // enum_flag_Category_IfArrayThenUnused                 = 0x00010000, // sub-category of Array
        enum_flag_Category_IfArrayThenSzArray                   = 0x00020000, // sub-category of Array

        enum_flag_Category_Interface        = 0x000C0000,
        enum_flag_Category_Unused_4         = 0x000D0000,
        enum_flag_Category_Unused_5         = 0x000E0000,
        enum_flag_Category_Unused_6         = 0x000F0000,

        enum_flag_Category_ElementTypeMask  = 0x000E0000, // bits that matter for element type mask

        enum_flag_HasFinalizer                = 0x00100000, // instances require finalization. GC depends on this bit.
        enum_flag_Collectible                 = 0x00200000, // GC depends on this bit.
        // enum_flag_unused                   = 0x00400000,

#ifdef FEATURE_64BIT_ALIGNMENT
        enum_flag_RequiresAlign8              = 0x00800000, // Type requires 8-byte alignment (only set on platforms that require this and don't get it implicitly)
#endif

        enum_flag_ContainsGCPointers          = 0x01000000, // Contains object references
        enum_flag_HasTypeEquivalence          = 0x02000000, // can be equivalent to another type
        enum_flag_IsTrackedReferenceWithFinalizer = 0x04000000,
        // unused                             = 0x08000000,

        enum_flag_IDynamicInterfaceCastable   = 0x10000000, // class implements IDynamicInterfaceCastable interface
        enum_flag_ContainsGenericVariables    = 0x20000000, // we cache this flag to help detect these efficiently and
                                                            // to detect this condition when restoring
        enum_flag_ComObject                   = 0x40000000, // class is a com object
        enum_flag_HasComponentSize            = 0x80000000, // This is set if component size is used for flags.

        // Types that require non-trivial interface cast have this bit set in the category
        enum_flag_NonTrivialInterfaceCast   =  enum_flag_Category_Array
                                             | enum_flag_ComObject
                                             | enum_flag_IDynamicInterfaceCastable
                                             | enum_flag_Category_ValueType

    };  // enum WFLAGS_HIGH_ENUM

// NIDump needs to be able to see these flags
// TODO: figure out how to make these private
#if defined(DACCESS_COMPILE)
public:
#else
private:
#endif
    enum WFLAGS2_ENUM
    {
        // AS YOU ADD NEW FLAGS PLEASE CONSIDER WHETHER Generics::NewInstantiation NEEDS
        // TO BE UPDATED IN ORDER TO ENSURE THAT METHODTABLES DUPLICATED FOR GENERIC INSTANTIATIONS
        // CARRY THE CORECT FLAGS.

        enum_flag_HasPerInstInfo            = 0x0001,
        enum_flag_DynamicStatics            = 0x0002,
        enum_flag_HasDispatchMapSlot        = 0x0004,

        enum_flag_wflags2_unused_2          = 0x0008,
        //unused                            = 0x0010,
        enum_flag_IsIntrinsicType           = 0x0020,
        enum_flag_HasCctor                  = 0x0040,
        enum_flag_HasVirtualStaticMethods   = 0x0080,

        enum_flag_TokenMask                 = 0xFFFFFF00,
    };  // enum WFLAGS2_ENUM

    __forceinline void ClearFlag(WFLAGS_LOW_ENUM flag)
    {
        _ASSERTE(!IsStringOrArray());
        m_dwFlags &= ~flag;
    }
    __forceinline void SetFlag(WFLAGS_LOW_ENUM flag)
    {
        _ASSERTE(!IsStringOrArray());
        m_dwFlags |= flag;
    }
    __forceinline DWORD GetFlag(WFLAGS_LOW_ENUM flag) const
    {
        SUPPORTS_DAC;
        return (IsStringOrArray() ? (enum_flag_StringArrayValues & flag) : (m_dwFlags & flag));
    }
    __forceinline BOOL TestFlagWithMask(WFLAGS_LOW_ENUM mask, WFLAGS_LOW_ENUM flag) const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (IsStringOrArray() ? (((DWORD)enum_flag_StringArrayValues & (DWORD)mask) == (DWORD)flag) :
            ((m_dwFlags & (DWORD)mask) == (DWORD)flag));
    }

    __forceinline void ClearFlag(WFLAGS_HIGH_ENUM flag)
    {
        m_dwFlags &= ~flag;
    }
    __forceinline void SetFlag(WFLAGS_HIGH_ENUM flag)
    {
        m_dwFlags |= flag;
    }
    __forceinline DWORD GetFlag(WFLAGS_HIGH_ENUM flag) const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_dwFlags & flag;
    }
    __forceinline BOOL TestFlagWithMask(WFLAGS_HIGH_ENUM mask, WFLAGS_HIGH_ENUM flag) const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return ((m_dwFlags & (DWORD)mask) == (DWORD)flag);
    }

    __forceinline void ClearFlag(WFLAGS2_ENUM flag)
    {
        m_dwFlags2 &= ~flag;
    }
    __forceinline void SetFlag(WFLAGS2_ENUM flag)
    {
        m_dwFlags2 |= flag;
    }
    __forceinline DWORD GetFlag(WFLAGS2_ENUM flag) const
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_dwFlags2 & flag;
    }
    __forceinline BOOL TestFlagWithMask(WFLAGS2_ENUM mask, WFLAGS2_ENUM flag) const
    {
        return (m_dwFlags2 & (DWORD)mask) == (DWORD)flag;
    }

#ifndef DACCESS_COMPILE
    void SetNullableDetails(UINT16 offsetToValueField, UINT32 sizeOfValueField)
    {
        STANDARD_VM_CONTRACT;
        _ASSERTE(IsNullable());
#ifndef TARGET_64BIT
        if (sizeOfValueField > 0xFFFFFF)
        {
            // We can't encode the size of the value field in the Nullable<T> MethodTable
            // because it's too large. This is a limitation of the encoding. It is not expected
            // to impact any real customers, as Nullable<T> should only be used on the stack
            // where having a 16MB local would always be a significant problem. Especially oh
            // a 32-bit machine.
            ThrowHR(COR_E_TYPELOAD);
        }
        if (offsetToValueField > 255)
        {
            // If we get here something completely unexpected has happened. We don't expect alignment greater than 128
            ThrowHR(COR_E_TYPELOAD);
        }
        m_encodedNullableUnboxData = ((TADDR)sizeOfValueField << 8) | (TADDR)offsetToValueField;
#else
        m_encodedNullableUnboxData = ((TADDR)sizeOfValueField << 32) | (TADDR)offsetToValueField;
#endif
    }
#endif // DACCESS_COMPILE

private:
    // Low WORD is component size for array and string types (HasComponentSize() returns true).
    // Used for flags otherwise.
    DWORD           m_dwFlags;

    // Base size of instance of this class when allocated on the heap
    DWORD           m_BaseSize;

    // See WFLAGS2_ENUM for values.
    DWORD           m_dwFlags2;

    // <NICE> In the normal cases we shouldn't need a full word for each of these </NICE>
    WORD            m_wNumVirtuals;
    WORD            m_wNumInterfaces;

#ifdef _DEBUG
    LPCUTF8         debug_m_szClassName;
#endif //_DEBUG

    PTR_MethodTable m_pParentMethodTable;

    PTR_Module      m_pModule;

    PTR_MethodTableAuxiliaryData m_pAuxiliaryData;

    // The value of lowest two bits describe what the union contains
    enum LowBits {
        UNION_EECLASS      = 0,    //  0 - pointer to EEClass. This MethodTable is the canonical method table.
        UNION_METHODTABLE  = 1,    //  1 - pointer to canonical MethodTable.
    };
    static const TADDR UNION_MASK = 1;

    union {
        DPTR(EEClass) m_pEEClass;
        TADDR m_pCanonMT;
    };

    __forceinline static LowBits union_getLowBits(TADDR pCanonMT)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return LowBits(pCanonMT & UNION_MASK);
    }
    __forceinline static TADDR   union_getPointer(TADDR pCanonMT)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return (pCanonMT & ~UNION_MASK);
    }

    // m_pPerInstInfo and m_pInterfaceMap have to be at fixed offsets because of performance sensitive
    // JITed code and JIT helpers. The space used by m_pPerInstInfo is used to represent the array
    // element type handle for array MethodTables.

    public:
    union
    {
        PerInstInfo_t m_pPerInstInfo;
        TADDR         m_ElementTypeHnd;
    };
    union
    {
        PTR_InterfaceInfo   m_pInterfaceMap;
        TADDR               m_encodedNullableUnboxData; // Used for Nullable<T> to represent the offset to the value field, and the size of the value field
    };

    // VTable slots go here

    // Optional Members go here
    //    See above for the list of optional members

    // Generic dictionary pointers go here

    // Interface map goes here

    // Generic instantiation+dictionary goes here

private:

    // disallow direct creation
    void *operator new(size_t dummy);
    void operator delete(void *pData);
    MethodTable();

    PTR_GenericsStaticsInfo GetGenericsStaticsInfo()
    {
        PTR_MethodTableAuxiliaryData AuxiliaryData = GetAuxiliaryDataForWrite();
        _ASSERTE(HasGenericsStaticsInfo());
        return MethodTableAuxiliaryData::GetGenericStaticsInfo(AuxiliaryData);
    }

public:
    PTR_DynamicStaticsInfo GetDynamicStaticsInfo()
    {
        PTR_MethodTableAuxiliaryData AuxiliaryData = GetAuxiliaryDataForWrite();
        _ASSERTE(IsDynamicStatics());
        return MethodTableAuxiliaryData::GetDynamicStaticsInfo(AuxiliaryData);
    }

    PTR_ThreadStaticsInfo GetThreadStaticsInfo()
    {
        PTR_MethodTableAuxiliaryData AuxiliaryData = GetAuxiliaryDataForWrite();
        _ASSERTE(GetNumThreadStaticFields() > 0);
        return MethodTableAuxiliaryData::GetThreadStaticsInfo(AuxiliaryData);
    }
private:

    // Optional members.  These are used for fields in the data structure where
    // the fields are (a) known when MT is created and (b) there is a default
    // value for the field in the common case.  That is, they are normally used
    // for data that is only relevant to a small number of method tables.
    // - Optional members can accommodate structures of any size. It is trivial to add new ones,
    //   the access is somewhat slow.

    // The following macro will automatically create GetXXX accessors for the optional members.
#define METHODTABLE_OPTIONAL_MEMBERS() \
    /*                          NAME                    TYPE                            GETTER                     */ \
    /* Accessed during certain generic type load operations only, so low priority */                                  \
    METHODTABLE_OPTIONAL_MEMBER(ExtraInterfaceInfo,     TADDR,                          GetExtraInterfaceInfoPtr    ) \

    enum OptionalMemberId
    {
#undef METHODTABLE_OPTIONAL_MEMBER
#define METHODTABLE_OPTIONAL_MEMBER(NAME, TYPE, GETTER) OptionalMember_##NAME,
        METHODTABLE_OPTIONAL_MEMBERS()
        OptionalMember_Count,

        OptionalMember_First = OptionalMember_ExtraInterfaceInfo,
    };

    FORCEINLINE DWORD GetOffsetOfOptionalMember(OptionalMemberId id);

public:

    //
    // Public accessor helpers for the optional members of MethodTable
    //

#undef METHODTABLE_OPTIONAL_MEMBER
#define METHODTABLE_OPTIONAL_MEMBER(NAME, TYPE, GETTER) \
    inline DPTR(TYPE) GETTER() \
    { \
        LIMITED_METHOD_CONTRACT; \
        _ASSERTE(Has##NAME()); \
        return dac_cast<DPTR(TYPE)>(dac_cast<TADDR>(this) + GetOffsetOfOptionalMember(OptionalMember_##NAME)); \
    }

    METHODTABLE_OPTIONAL_MEMBERS()

private:
    inline DWORD GetStartOffsetOfOptionalMembers()
    {
        WRAPPER_NO_CONTRACT;
        return GetOffsetOfOptionalMember(OptionalMember_First);
    }

    inline DWORD GetEndOffsetOfOptionalMembers()
    {
        WRAPPER_NO_CONTRACT;
        return GetOffsetOfOptionalMember(OptionalMember_Count);
    }

    inline static DWORD GetOptionalMembersAllocationSize(bool hasInterfaceMap);
    inline DWORD GetOptionalMembersSize();

    // The PerInstInfo is a (possibly empty) array of pointers to
    // Instantiations/Dictionaries. This array comes after the optional members.
    inline DWORD GetPerInstInfoSize();

    // This is the size of the interface map chunk in the method table.
    // If the MethodTable has a dynamic interface map then the size includes the pointer
    // that stores the extra info for that map.
    // The interface map itself comes after the PerInstInfo (if any)
    inline DWORD GetInterfaceMapSize();

    // The instantiation/dictionary comes at the end of the MethodTable after
    // the interface map. This is the total number of bytes used by the dictionary.
    // The pSlotSize argument is used to return the size occupied by slots (not including
    // the optional back pointer used when expanding dictionaries).
    inline DWORD GetInstAndDictSize(DWORD *pSlotSize);

public:

    BOOL Validate ();

    static void GetStaticsOffsets(StaticsOffsetType staticsOffsetType, bool fGenericsStatics, uint32_t *dwGCOffset, uint32_t *dwNonGCOffset);

    friend struct ::cdac_data<MethodTable>;
};  // class MethodTable

template<> struct cdac_data<MethodTable>
{
    static constexpr size_t MTFlags = offsetof(MethodTable, m_dwFlags);
    static constexpr size_t BaseSize = offsetof(MethodTable, m_BaseSize);
    static constexpr size_t MTFlags2 = offsetof(MethodTable, m_dwFlags2);
    static constexpr size_t EEClassOrCanonMT = offsetof(MethodTable, m_pEEClass);
    static constexpr size_t Module = offsetof(MethodTable, m_pModule);
    static constexpr size_t AuxiliaryData = offsetof(MethodTable, m_pAuxiliaryData);
    static constexpr size_t ParentMethodTable = offsetof(MethodTable, m_pParentMethodTable);
    static constexpr size_t NumInterfaces = offsetof(MethodTable, m_wNumInterfaces);
    static constexpr size_t NumVirtuals = offsetof(MethodTable, m_wNumVirtuals);
    static constexpr size_t PerInstInfo = offsetof(MethodTable, m_pPerInstInfo);
};

#ifndef CROSSBITNESS_COMPILE
static_assert_no_msg(sizeof(MethodTable) == SIZEOF__MethodTable_);
#endif
#if defined(FEATURE_TYPEEQUIVALENCE) && !defined(DACCESS_COMPILE)
WORD GetEquivalentMethodSlot(MethodTable * pOldMT, MethodTable * pNewMT, WORD wMTslot, BOOL *pfFound);
#endif // defined(FEATURE_TYPEEQUIVALENCE) && !defined(DACCESS_COMPILE)

MethodTable* CreateMinimalMethodTable(Module* pContainingModule,
                                      LoaderAllocator* pLoaderAllocator,
                                      AllocMemTracker* pamTracker);

void ThrowEntryPointNotFoundException(
    MethodTable* pTargetClass,
    MethodTable* pInterfaceMT,
    MethodDesc* pInterfaceMD);

void ThrowAmbiguousResolutionException(
    MethodTable* pTargetClass,
    MethodTable* pInterfaceMT,
    MethodDesc* pInterfaceMD);


#ifndef DACCESS_COMPILE
void DoNotRecordTheResultOfEnsureLoadLevel();
#endif

#endif // !_METHODTABLE_H_
