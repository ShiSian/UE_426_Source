// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	UObjectBase.cpp: Unreal UObject base class
=============================================================================*/

#include "UObject/UObjectBase.h"
#include "Misc/MessageDialog.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"
#include "HAL/LowLevelMemTracker.h"
#include "Misc/FeedbackContext.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectAllocator.h"
#include "UObject/UObjectHash.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Templates/Casts.h"
#include "UObject/GCObject.h"
#include "UObject/LinkerLoad.h"
#include "Misc/CommandLine.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/LoadTimeTrace.h"

DEFINE_LOG_CATEGORY_STATIC(LogUObjectBase, Log, All);
DEFINE_STAT(STAT_UObjectsStatGroupTester);

DECLARE_CYCLE_STAT(TEXT("CreateStatID"), STAT_CreateStatID, STATGROUP_StatSystem);

DEFINE_LOG_CATEGORY_STATIC(LogUObjectBootstrap, Display, Display);


/** Whether uobject system is initialized.												*/
namespace Internal
{
	static bool& GetUObjectSubsystemInitialised()
	{
		static bool ObjInitialized = false;
		return ObjInitialized;
	}
};
bool UObjectInitialized()
{
	return Internal::GetUObjectSubsystemInitialised();
}

/** Objects to automatically register once the object system is ready.					*/
struct FPendingRegistrantInfo
{
	//对象名字
	const TCHAR*	Name;
	//所属包的名字
	const TCHAR*	PackageName;
	FPendingRegistrantInfo(const TCHAR* InName,const TCHAR* InPackageName)
		:	Name(InName)
		,	PackageName(InPackageName)
	{}
	static TMap<UObjectBase*, FPendingRegistrantInfo>& GetMap()
	{
		//用对象指针做Key，这样才可以通过对象地址获得其名字信息，这个时候UClass对象本身其实还没有名字，要等之后的注册才能设置进去
		static TMap<UObjectBase*, FPendingRegistrantInfo> PendingRegistrantInfo;
		return PendingRegistrantInfo;
	}
};


/** Objects to automatically register once the object system is ready.					*/
struct FPendingRegistrant
{
	//对象指针，用该值去PendingRegistrants里查找名字。
	UObjectBase*	Object;
	//链表下一个节点
	FPendingRegistrant*	NextAutoRegister;
	FPendingRegistrant(UObjectBase* InObject)
	:	Object(InObject)
	,	NextAutoRegister(NULL)
	{}
};
//全局链表头
static FPendingRegistrant* GFirstPendingRegistrant = NULL;
//全局链表尾
static FPendingRegistrant* GLastPendingRegistrant = NULL;

#if USE_PER_MODULE_UOBJECT_BOOTSTRAP
static TMap<FName, TArray<FPendingRegistrant*>>& GetPerModuleBootstrapMap()
{
	static TMap<FName, TArray<FPendingRegistrant*>> PendingRegistrantInfo;
	return PendingRegistrantInfo;
}

#endif


/**
 * Constructor used for bootstrapping
 * @param	InClass			possibly NULL, this gives the class of the new object, if known at this time
 * @param	InFlags			RF_Flags to assign
 */
UObjectBase::UObjectBase( EObjectFlags InFlags )
:	ObjectFlags			(InFlags)
,	InternalIndex		(INDEX_NONE)
,	ClassPrivate		(nullptr)
,	OuterPrivate		(nullptr)
{}

/**
 * Constructor used by StaticAllocateObject
 * @param	InClass				non NULL, this gives the class of the new object, if known at this time
 * @param	InFlags				RF_Flags to assign
 * @param	InOuter				outer for this object
 * @param	InName				name of the new object
 * @param	InObjectArchetype	archetype to assign
 */
UObjectBase::UObjectBase(UClass* InClass, EObjectFlags InFlags, EInternalObjectFlags InInternalFlags, UObject *InOuter, FName InName)
:	ObjectFlags			(InFlags)
,	InternalIndex		(INDEX_NONE)
,	ClassPrivate		(InClass)
,	OuterPrivate		(InOuter)
{
	check(ClassPrivate);
	// Add to global table.
	AddObject(InName, InInternalFlags);
}


/**
 * Final destructor, removes the object from the object array, and indirectly, from any annotations
 **/
UObjectBase::~UObjectBase()
{
	// If not initialized, skip out.
	if( UObjectInitialized() && ClassPrivate && !GIsCriticalError )
	{
		// Validate it.
		check(IsValidLowLevel());
		check(GetFName() == NAME_None);
		GUObjectArray.FreeUObjectIndex(this);
	}
}




/**
 * Convert a boot-strap registered class into a real one, add to uobject array, etc *
 * @param UClassStaticClass Now that it is known, fill in UClass::StaticClass() as the class
 * 
 * 【对象真正注册的地方】 
 * DeferredRegister的含义（注册的具体含义）：
 * 1、Deferred是延迟的意思。
 *    区分于之前的UObjectBase::Register，
 *    延迟的意思是在对象系统初始化（GUObjectAllocator和GUObjectArray）之后的注册。
 * 	  Register的时候还不能正常NewObject和加载Package，
 * 	  而初始化之后这个阶段就可以开始正常的使用UObject系统的功能了。
 * 	  所以这里面才可以开始CreatePackage。
 * 2、Register注册。
 *    确定一点的意思是对代码里的class生成相应的UClass*对象并添加（注册）到全局对象数组里。
 * 
 * 所以总结起来这里所做的是创建出UClass*的Outer指向的Package，
 * 并设置ClassPrivate（这里都是UClass*对象，所以其实都是UClass::StaticClass()）。
 * 然后在AddObject里设置NamePrivate。因此这步之后这些一个个UClass*对象才有名字，之间的联系才算完整。
 * 但同时也需要注意的是，这些UClass*对象里仍然没有UProperty和UFunciton，下一篇来讲解这些的构造生成。
 */
void UObjectBase::DeferredRegister(UClass *UClassStaticClass,const TCHAR* PackageName,const TCHAR* InName)
{
	check(UObjectInitialized());
	// Set object properties.
	//创建属于的Package
	UPackage* Package = CreatePackage(PackageName);
	check(Package);
	Package->SetPackageFlags(PKG_CompiledIn);
	//设定Outer到该Package
	OuterPrivate = Package;

	check(UClassStaticClass);
	check(!ClassPrivate);
	//设定属于的UClass*类型
	ClassPrivate = UClassStaticClass;

	// Add to the global object table.
	//注册该对象的名字
	AddObject(FName(InName), EInternalObjectFlags::None);

	// Make sure that objects disregarded for GC are part of root set.
	check(!GUObjectArray.IsDisregardForGC(this) || GUObjectArray.IndexToObject(InternalIndex)->IsRootSet());

	UE_LOG(LogUObjectBootstrap, Verbose, TEXT("UObjectBase::DeferredRegister %s %s"), PackageName, InName);
}

/**
 * Add a newly created object to the name hash tables and the object array
 *
 * @param Name name to assign to this uobject
 */
void UObjectBase::AddObject(FName InName, EInternalObjectFlags InSetInternalFlags)
{
	//设定对象的名字
	NamePrivate = InName;
	EInternalObjectFlags InternalFlagsToSet = InSetInternalFlags;
	if (!IsInGameThread())
	{
		InternalFlagsToSet |= EInternalObjectFlags::Async;
	}
	if (ObjectFlags & RF_MarkAsRootSet)
	{		
		InternalFlagsToSet |= EInternalObjectFlags::RootSet;
		ObjectFlags &= ~RF_MarkAsRootSet;
	}
	if (ObjectFlags & RF_MarkAsNative)
	{
		InternalFlagsToSet |= EInternalObjectFlags::Native;
		ObjectFlags &= ~RF_MarkAsNative;
	}
	AllocateUObjectIndexForCurrentThread(this);
	check(InName != NAME_None && InternalIndex >= 0);
	if (InternalFlagsToSet != EInternalObjectFlags::None)
	{
		GUObjectArray.IndexToObject(InternalIndex)->SetFlags(InternalFlagsToSet);
	
	}	
	HashObject(this);
	check(IsValidLowLevel());
}

/**
 * Just change the FName and Outer and rehash into name hash tables. For use by higher level rename functions.
 *
 * @param NewName	new name for this object
 * @param NewOuter	new outer for this object, if NULL, outer will be unchanged
 */
void UObjectBase::LowLevelRename(FName NewName,UObject *NewOuter)
{
#if STATS || ENABLE_STATNAMEDEVENTS_UOBJECT
	((UObject*)this)->ResetStatID(); // reset the stat id since this thing now has a different name
#endif
	UnhashObject(this);
	check(InternalIndex >= 0);
	NamePrivate = NewName;
	if (NewOuter)
	{
		OuterPrivate = NewOuter;
	}
	HashObject(this);
}

UPackage* UObjectBase::GetExternalPackage() const
{
	// if we have no outer, consider this a package, packages returns themselves as their external package
	if (OuterPrivate == nullptr)
	{
		return CastChecked<UPackage>((UObject*)(this));
	}
	UPackage* ExternalPackage = nullptr;
	if ((GetFlags() & RF_HasExternalPackage) != 0)
	{
		ExternalPackage = GetObjectExternalPackageThreadSafe(this);
		// if the flag is set there should be an override set.
		ensure(ExternalPackage);
	}
	return ExternalPackage;
}

UPackage* UObjectBase::GetExternalPackageInternal() const
{
	// if we have no outer, consider this a package, packages returns themselves as their external package
	if (OuterPrivate == nullptr)
	{
		return CastChecked<UPackage>((UObject*)(this));
	}
	return (GetFlags() & RF_HasExternalPackage) != 0 ? GetObjectExternalPackageInternal(this) : nullptr;
}

void UObjectBase::SetExternalPackage(UPackage* InPackage)
{
	HashObjectExternalPackage(this, InPackage);
	if (InPackage)
	{
		SetFlagsTo(GetFlags() | RF_HasExternalPackage);
	}
	else
	{
		SetFlagsTo(GetFlags() & ~RF_HasExternalPackage);
	}
}

void UObjectBase::SetClass(UClass* NewClass)
{
#if STATS || ENABLE_STATNAMEDEVENTS_UOBJECT
	((UObject*)this)->ResetStatID(); // reset the stat id since this thing now has a different name
#endif

	UnhashObject(this);
#if USE_UBER_GRAPH_PERSISTENT_FRAME
	UClass* OldClass = ClassPrivate;
	ClassPrivate->DestroyPersistentUberGraphFrame((UObject*)this);
#endif
	ClassPrivate = NewClass;
#if USE_UBER_GRAPH_PERSISTENT_FRAME
	ClassPrivate->CreatePersistentUberGraphFrame((UObject*)this, /*bCreateOnlyIfEmpty =*/false, /*bSkipSuperClass =*/false, OldClass);
#endif
	HashObject(this);
}


/**
 * Checks to see if the object appears to be valid
 * @return true if this appears to be a valid object
 */
bool UObjectBase::IsValidLowLevel() const
{
	if( this == nullptr )
	{
		UE_LOG(LogUObjectBase, Warning, TEXT("NULL object") );
		return false;
	}
	if( !ClassPrivate )
	{
		UE_LOG(LogUObjectBase, Warning, TEXT("Object is not registered") );
		return false;
	}
	return GUObjectArray.IsValid(this);
}

bool UObjectBase::IsValidLowLevelFast(bool bRecursive /*= true*/) const
{
	// As DEFAULT_ALIGNMENT is defined to 0 now, I changed that to the original numerical value here
	const int32 AlignmentCheck = MIN_ALIGNMENT - 1;

	// Check 'this' pointer before trying to access any of the Object's members
	if ((this == nullptr) || (UPTRINT)this < 0x100)
	{
		UE_LOG(LogUObjectBase, Error, TEXT("\'this\' pointer is invalid."));
		return false;
	}
	if ((UPTRINT)this & AlignmentCheck)
	{
		UE_LOG(LogUObjectBase, Error, TEXT("\'this\' pointer is misaligned."));
		return false;
	}
	if (*(void**)this == nullptr)
	{
		UE_LOG(LogUObjectBase, Error, TEXT("Virtual functions table is invalid."));
		return false;
	}

	// These should all be 0.
	const UPTRINT CheckZero = (ObjectFlags & ~RF_AllFlags) | ((UPTRINT)ClassPrivate & AlignmentCheck) | ((UPTRINT)OuterPrivate & AlignmentCheck);
	if (!!CheckZero)
	{
		UE_LOG(LogUObjectBase, Error, TEXT("Object flags are invalid or either Class or Outer is misaligned"));
		return false;
	}
	// These should all be non-NULL (except CDO-alignment check which should be 0)
	if (ClassPrivate == nullptr || ClassPrivate->ClassDefaultObject == nullptr || ((UPTRINT)ClassPrivate->ClassDefaultObject & AlignmentCheck) != 0)
	{
		UE_LOG(LogUObjectBase, Error, TEXT("Class pointer is invalid or CDO is invalid."));
		return false;
	}
	// Avoid infinite recursion so call IsValidLowLevelFast on the class object with bRecirsive = false.
	if (bRecursive && !ClassPrivate->IsValidLowLevelFast(false))
	{
		UE_LOG(LogUObjectBase, Error, TEXT("Class object failed IsValidLowLevelFast test."));
		return false;
	}
	// Lightweight versions of index checks.
	if (!GUObjectArray.IsValidIndex(this) || !NamePrivate.IsValidIndexFast())
	{
		UE_LOG(LogUObjectBase, Error, TEXT("Object array index or name index is invalid."));
		return false;
	}
	return true;
}

void UObjectBase::EmitBaseReferences(UClass *RootClass)
{
	static const FName ClassPropertyName(TEXT("Class"));
	static const FName OuterPropertyName(TEXT("Outer"));
	// Mark UObject class reference as persistent object reference so that it (ClassPrivate) doesn't get nulled when a class
	// is marked as pending kill. Nulling ClassPrivate may leave the object in a broken state if it doesn't get GC'd in the same
	// GC call as its class. And even if it gets GC'd in the same call as its class it may break inside of GC (for example when traversing TMap references)
	RootClass->EmitObjectReference(STRUCT_OFFSET(UObjectBase, ClassPrivate), ClassPropertyName, GCRT_PersistentObject);
	RootClass->EmitObjectReference(STRUCT_OFFSET(UObjectBase, OuterPrivate), OuterPropertyName, GCRT_PersistentObject);
	RootClass->EmitExternalPackageReference();
}

#if USE_PER_MODULE_UOBJECT_BOOTSTRAP
static void UObjectReleaseModuleRegistrants(FName Module)
{
	TMap<FName, TArray<FPendingRegistrant*>>& PerModuleMap = GetPerModuleBootstrapMap();

	FName Package = IPluginManager::Get().PackageNameFromModuleName(Module);

	FName ScriptName = *(FString(TEXT("/Script/")) + Package.ToString());

	TArray<FPendingRegistrant*>* Array = PerModuleMap.Find(ScriptName);
	if (Array)
	{
		SCOPED_BOOT_TIMING("UObjectReleaseModuleRegistrants");
		for (FPendingRegistrant* PendingRegistration : *Array)
		{
			if (GLastPendingRegistrant)
			{
				GLastPendingRegistrant->NextAutoRegister = PendingRegistration;
			}
			else
			{
				check(!GFirstPendingRegistrant);
				GFirstPendingRegistrant = PendingRegistration;
			}
			GLastPendingRegistrant = PendingRegistration;
		}
		UE_LOG(LogUObjectBootstrap, Verbose, TEXT("UObjectReleaseModuleRegistrants %d items in %s"), Array->Num(), *ScriptName.ToString());
		PerModuleMap.Remove(ScriptName);
	}
	else
	{
		UE_LOG(LogUObjectBootstrap, Verbose, TEXT("UObjectReleaseModuleRegistrants no items in %s"), *ScriptName.ToString());
	}
}

void UObjectReleaseAllModuleRegistrants()
{
	SCOPED_BOOT_TIMING("UObjectReleaseAllModuleRegistrants");
	TMap<FName, TArray<FPendingRegistrant*>>& PerModuleMap = GetPerModuleBootstrapMap();
	for (auto& Pair : PerModuleMap)
	{
		for (FPendingRegistrant* PendingRegistration : Pair.Value)
		{
			if (GLastPendingRegistrant)
			{
				GLastPendingRegistrant->NextAutoRegister = PendingRegistration;
			}
			else
			{
				check(!GFirstPendingRegistrant);
				GFirstPendingRegistrant = PendingRegistration;
			}
			GLastPendingRegistrant = PendingRegistration;
		}
		UE_LOG(LogUObjectBootstrap, Verbose, TEXT("UObjectReleaseAllModuleRegistrants %d items in %s"), Pair.Value.Num(), *Pair.Key.ToString());
	}
	PerModuleMap.Empty();
	ProcessNewlyLoadedUObjects();
}

static void DumpPendingUObjectModules(const TArray<FString>& Args)
{
	TMap<FName, TArray<FPendingRegistrant*>>& PerModuleMap = GetPerModuleBootstrapMap();
	for (auto& Pair : PerModuleMap)
	{
		UE_LOG(LogUObjectBootstrap, Display, TEXT("Not yet loaded: %d items in %s"), Pair.Value.Num(), *Pair.Key.ToString());
	}
}

static FAutoConsoleCommand DumpPendingUObjectModulesCmd(
	TEXT("DumpPendingUObjectModules"),
	TEXT("When doing per-module UObject bootstrapping, show the modules that are not yet loaded."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&DumpPendingUObjectModules)
);

#endif

/** 
Enqueue the registration for this object. 
该函数比较简单，只是简单的先记录一下信息到一个全局单例Map里和一个全局链表里

1、为何该函数先记录一下信息？

	因为UClass的注册分成了多步，在static初始化的时候（连main都没进去呢），
	甚至到后面CoreUObject模块加载的时候，
	UObject对象分配索引的机制（GUObjectAllocator和GUObjectArray）还没有初始化完毕，
	因此这个时候如果走下一步去创建各种UProperty、UFunction或UPackage是不合适，
	创建出来了也没有合适的地方来保存索引。
	所以，在最开始的时候，只能先简单的创建出各UClass*对象（简单到对象的名字都还没有设定，更何况填充里面的属性和方法了），
	先在内存里把这些UClass*对象记录一下，等后续对象的存储结构准备好了，
	就可以把这些UClass*对象再拉出来继续构造了。
	在后续操作中这些对象会被消费到（初始化对象存储机制的函数调用是InitUObject()、继续构造的操作是在ProcessNewlyLoadedUObjects()里

2、记录信息为何需要一个TMap加一个链表？

	我们可以看到，为了记录信息，明明是用一个数据结构就能保存的（源码里的两个数据结构里的数据数量也是1:1的），为何要麻烦的设置成这样。
	原因有三：
	1）是快速查找的需要。
		在后续的别的代码（获取CDO等）里也会经常调用到UObjectForceRegistration(NewClass)，
		因此常常有通过一个对象指针来查找注册信息的需要，
		这个时候为了性能就必须要用字典类的数据结构才能做到O(1)的查找。
	2）顺序注册的需要。
		字典类的数据结构一般来说内部为了hash，数据遍历取出的顺序无法保证和添加的顺序一致，
		而我们又想要遵循添加的顺序来注册
		（很合理，早添加进来的是早加载的，是更底层的，处在依赖顺序的前提位置。我们前面的SuperClass和WithinClass的访问也表明了这一点），
		因此就需要另一个顺序数据结构来辅助。
	3）那为什么是链表而不是数组呢？
		链表比数组优势的地方也只在于可以快速的中间插入。
		但是UE源码里也没有这个方面的体现，所以其实二者都可以。
		实际上在源码里把注册结构改为用数组也依然可以正常工作。
*/
void UObjectBase::Register(const TCHAR* PackageName,const TCHAR* InName)
{
	//1、添加到全局单例Map里，用对象指针做Key，Value是对象的名字和所属包的名字。
	TMap<UObjectBase*, FPendingRegistrantInfo>& PendingRegistrants = FPendingRegistrantInfo::GetMap();	
	PendingRegistrants.Add(this, FPendingRegistrantInfo(InName, PackageName));

	//2、将本对象添加到全局链表里（这里与上一句换了下位置）
	//2-1、使用该对象构造一个链表节点
	FPendingRegistrant* PendingRegistration = new FPendingRegistrant(this);

#if USE_PER_MODULE_UOBJECT_BOOTSTRAP
	if (FName(PackageName) != FName("/Script/CoreUObject"))
	{
		TMap<FName, TArray<FPendingRegistrant*>>& PerModuleMap = GetPerModuleBootstrapMap();

		PerModuleMap.FindOrAdd(FName(PackageName)).Add(PendingRegistration);
	}
	else
#endif
	{
		if (GLastPendingRegistrant)
		{
			//2-2、 全局链表尾非空，直接设置全局链表为给该链表节点
			GLastPendingRegistrant->NextAutoRegister = PendingRegistration;
		}
		else
		{
			//2-2、全局链表尾为空，检查全局链表头是否有效，如果有效设置全局链表头为该链表节点，否则触发断言
			check(!GFirstPendingRegistrant);
			GFirstPendingRegistrant = PendingRegistration;
		}
		GLastPendingRegistrant = PendingRegistration;
	}
}


/**
 * Dequeues registrants from the list of pending registrations into an array.
 * The contents of the array is preserved, and the new elements are appended.
 */
static void DequeuePendingAutoRegistrants(TArray<FPendingRegistrant>& OutPendingRegistrants)
{
	// We process registrations in the order they were enqueued, since each registrant ensures
	// its dependencies are enqueued before it enqueues itself.
	FPendingRegistrant* NextPendingRegistrant = GFirstPendingRegistrant;
	GFirstPendingRegistrant = NULL;
	GLastPendingRegistrant = NULL;
	while(NextPendingRegistrant)
	{
		FPendingRegistrant* PendingRegistrant = NextPendingRegistrant;
		OutPendingRegistrants.Add(*PendingRegistrant);
		NextPendingRegistrant = PendingRegistrant->NextAutoRegister;
		delete PendingRegistrant;
	};
}

/**
 * Process the auto register objects adding them to the UObject array
 * 
 * 这个函数的主要目的是从GFirstPendingRegistrant和GLastPendingRegistrant
 * 定义的链表抽取出来FPendingRegistrant的列表，然后用UObjectForceRegistration来注册。
 * 但是要注意在每一项注册之后，都要重复调用DequeuePendingAutoRegistrants一下来继续提取，
 * 这么做是因为在真正注册一个UObject的时候（创建CDO和加载Package有可能引用到别的模块里的东西)，
 * 里面有可能触发另一个Module的加载，从而导致有新的注册项进来。
 * 所以就需要不断的提取注册直到把所有处理完。
 */
static void UObjectProcessRegistrants()
{
	SCOPED_BOOT_TIMING("UObjectProcessRegistrants");

	check(UObjectInitialized());
	// Make list of all objects to be registered.
	TArray<FPendingRegistrant> PendingRegistrants;
	//从链表中提取注册项列表
	DequeuePendingAutoRegistrants(PendingRegistrants);

	for(int32 RegistrantIndex = 0;RegistrantIndex < PendingRegistrants.Num();++RegistrantIndex)
	{
		const FPendingRegistrant& PendingRegistrant = PendingRegistrants[RegistrantIndex];
		//真正的注册
		UObjectForceRegistration(PendingRegistrant.Object, false);
		
		// should have been set by DeferredRegister
		check(PendingRegistrant.Object->GetClass()); 

		// Register may have resulted in new pending registrants being enqueued, so dequeue those.
		//继续尝试提取
		DequeuePendingAutoRegistrants(PendingRegistrants);
	}
}

/**
* 需要注意的是，UObjectForceRegistration这个函数有可能在多个地方调用：
*  1、在UObjectProcessRegistrants里对一个个对象手动进行注册。
*  2、UClass::CreateDefaultObject()内部用UObjectForceRegistration(ParentClass)
*     来确认基类已经注册完成。 
*  3、UE4CodeGen_Private::ConstructUClass()等构造类型对象的函数里用
*     UObjectForceRegistration(NewClass)来保证该对象已经注册。
* 所以，在重复的调用的时候，需要先判断是否PendingRegistrants里还存在该元素。
*/
void UObjectForceRegistration(UObjectBase* Object, bool bCheckForModuleRelease)
{
	//得到对象的注册信息
	TMap<UObjectBase*, FPendingRegistrantInfo>& PendingRegistrants = FPendingRegistrantInfo::GetMap();
	FPendingRegistrantInfo* Info = PendingRegistrants.Find(Object);
	//有可能为空，因为之前已经被注册过了
	if (Info)
	{
		//对象所在的Package
		const TCHAR* PackageName = Info->PackageName;
#if USE_PER_MODULE_UOBJECT_BOOTSTRAP
		if (bCheckForModuleRelease)
		{
			UObjectReleaseModuleRegistrants(FName(PackageName));
		}
#endif
		//对象名字
		const TCHAR* Name = Info->Name;
		//删除
		// delete this first so that it doesn't try to do it twice
		PendingRegistrants.Remove(Object); 
		//延迟注册
		Object->DeferredRegister(UClass::StaticClass(),PackageName,Name);
	}
}

/**
 * Struct containing the function pointer and package name of a UStruct to be registered with UObject system
 */
struct FPendingStructRegistrant
{	
	class UScriptStruct *(*RegisterFn)();
	const TCHAR* PackageName;

	FPendingStructRegistrant() {}
	FPendingStructRegistrant(class UScriptStruct *(*Fn)(), const TCHAR* InPackageName)
		: RegisterFn(Fn)
		, PackageName(InPackageName)
	{
	}
	FORCEINLINE bool operator==(const FPendingStructRegistrant& Other) const
	{
		return RegisterFn == Other.RegisterFn;
	}
};

static TArray<FPendingStructRegistrant>& GetDeferredCompiledInStructRegistration()
{
	static TArray<FPendingStructRegistrant> DeferredCompiledInRegistration;
	return DeferredCompiledInRegistration;
}

TMap<FName, UScriptStruct *(*)()>& GetDynamicStructMap()
{
	static TMap<FName, UScriptStruct *(*)()> DynamicStructMap;
	return DynamicStructMap;
}

void UObjectCompiledInDeferStruct(class UScriptStruct *(*InRegister)(), const TCHAR* PackageName, const TCHAR* ObjectName, bool bDynamic, const TCHAR* DynamicPathName)
{
	if (!bDynamic)
	{
		// we do reregister StaticStruct in hot reload
		FPendingStructRegistrant Registrant(InRegister, PackageName);
		checkSlow(!GetDeferredCompiledInStructRegistration().Contains(Registrant));
		GetDeferredCompiledInStructRegistration().Add(Registrant);
	}
	else
	{
		GetDynamicStructMap().Add(DynamicPathName, InRegister);
	}
	NotifyRegistrationEvent(PackageName, ObjectName, ENotifyRegistrationType::NRT_Struct, ENotifyRegistrationPhase::NRP_Added, (UObject *(*)())(InRegister), bDynamic);

}

class UScriptStruct *GetStaticStruct(class UScriptStruct *(*InRegister)(), UObject* StructOuter, const TCHAR* StructName, SIZE_T Size, uint32 Crc)
{
	NotifyRegistrationEvent(*StructOuter->GetOutermost()->GetName(), StructName, ENotifyRegistrationType::NRT_Struct, ENotifyRegistrationPhase::NRP_Started);
	UScriptStruct *Result = (*InRegister)();
	NotifyRegistrationEvent(*StructOuter->GetOutermost()->GetName(), StructName, ENotifyRegistrationType::NRT_Struct, ENotifyRegistrationPhase::NRP_Finished);
	return Result;
}

/**
 * Struct containing the function pointer and package name of a UEnum to be registered with UObject system
 */
struct FPendingEnumRegistrant
{
	class UEnum *(*RegisterFn)();
	const TCHAR* PackageName;

	FPendingEnumRegistrant() {}
	FPendingEnumRegistrant(class UEnum *(*Fn)(), const TCHAR* InPackageName)
		: RegisterFn(Fn)
		, PackageName(InPackageName)
	{
	}
	FORCEINLINE bool operator==(const FPendingEnumRegistrant& Other) const
	{
		return RegisterFn == Other.RegisterFn;
	}
};

// Same thing as GetDeferredCompiledInStructRegistration but for UEnums declared in header files without UClasses.
static TArray<FPendingEnumRegistrant>& GetDeferredCompiledInEnumRegistration()
{
	static TArray<FPendingEnumRegistrant> DeferredCompiledInRegistration;
	return DeferredCompiledInRegistration;
}

TMap<FName, UEnum *(*)()>& GetDynamicEnumMap()
{
	static TMap<FName, UEnum *(*)()> DynamicEnumMap;
	return DynamicEnumMap;
}

void UObjectCompiledInDeferEnum(class UEnum *(*InRegister)(), const TCHAR* PackageName, const TCHAR* ObjectName, bool bDynamic, const TCHAR* DynamicPathName)
{
	if (!bDynamic)
	{
		// we do reregister StaticStruct in hot reload
		FPendingEnumRegistrant Registrant(InRegister, PackageName);
		checkSlow(!GetDeferredCompiledInEnumRegistration().Contains(Registrant));
		GetDeferredCompiledInEnumRegistration().Add(Registrant);
	}
	else
	{
		GetDynamicEnumMap().Add(DynamicPathName, InRegister);
	}
	NotifyRegistrationEvent(PackageName, ObjectName, ENotifyRegistrationType::NRT_Enum, ENotifyRegistrationPhase::NRP_Added, (UObject *(*)())(InRegister), bDynamic);
}

class UEnum *GetStaticEnum(class UEnum *(*InRegister)(), UObject* EnumOuter, const TCHAR* EnumName)
{
	NotifyRegistrationEvent(*EnumOuter->GetOutermost()->GetName(), EnumName, ENotifyRegistrationType::NRT_Enum, ENotifyRegistrationPhase::NRP_Started);
	UEnum *Result = (*InRegister)();
	NotifyRegistrationEvent(*EnumOuter->GetOutermost()->GetName(), EnumName, ENotifyRegistrationType::NRT_Enum, ENotifyRegistrationPhase::NRP_Finished);
	return Result;
}

static TArray<class UClass *(*)()>& GetDeferredCompiledInRegistration()
{
	static TArray<class UClass *(*)()> DeferredCompiledInRegistration;
	return DeferredCompiledInRegistration;
}

/** Classes loaded with a module, deferred until we register them all in one go */
//返回可变引用
static TArray<FFieldCompiledInInfo*>& GetDeferredClassRegistration()
{
	//单例模式
	static TArray<FFieldCompiledInInfo*> DeferredClassRegistration;
	return DeferredClassRegistration;
}

#if WITH_HOT_RELOAD
/** Map of deferred class registration info (including size and reflection info) */
static TMap<FName, FFieldCompiledInInfo*>& GetDeferRegisterClassMap()
{
	static TMap<FName, FFieldCompiledInInfo*> DeferRegisterClassMap;
	return DeferRegisterClassMap;
}

/** Classes that changed during hot-reload and need to be re-instanced */
static TArray<FFieldCompiledInInfo*>& GetHotReloadClasses()
{
	static TArray<FFieldCompiledInInfo*> HotReloadClasses;
	return HotReloadClasses;
}
#endif

/** Removes prefix from the native class name */
FString UObjectBase::RemoveClassPrefix(const TCHAR* ClassName)
{
	static const TCHAR* DeprecatedPrefix = TEXT("DEPRECATED_");
	FString NameWithoutPrefix(ClassName);
	NameWithoutPrefix.MidInline(1, MAX_int32, false);
	if (NameWithoutPrefix.StartsWith(DeprecatedPrefix))
	{
		NameWithoutPrefix.MidInline(FCString::Strlen(DeprecatedPrefix), MAX_int32, false);
	}
	return NameWithoutPrefix;
}

//收集类名字，类大小，CRC信息， 并把类信息添加到静态数组DeferredClassRegistration中
void UClassCompiledInDefer(FFieldCompiledInInfo* ClassInfo, const TCHAR* Name, SIZE_T ClassSize, uint32 Crc)
{
	const FName CPPClassName = Name;
#if WITH_HOT_RELOAD
	// Check for existing classes
	TMap<FName, FFieldCompiledInInfo*>& DeferMap = GetDeferRegisterClassMap();
	FFieldCompiledInInfo** ExistingClassInfo = DeferMap.Find(CPPClassName);
	ClassInfo->bHasChanged = !ExistingClassInfo || (*ExistingClassInfo)->Size != ClassInfo->Size || (*ExistingClassInfo)->Crc != ClassInfo->Crc;
	if (ExistingClassInfo)
	{
		// Class exists, this can only happen during hot-reload
		checkf(GIsHotReload, TEXT("Trying to recreate class '%s' outside of hot reload!"), *CPPClassName.ToString());

		// Get the native name
		FString NameWithoutPrefix = UObjectBase::RemoveClassPrefix(Name);
		UClass* ExistingClass = FindObjectChecked<UClass>(ANY_PACKAGE, *NameWithoutPrefix);

		if (ClassInfo->bHasChanged)
		{
			// Rename the old class and move it to transient package
			ExistingClass->RemoveFromRoot();
			ExistingClass->ClearFlags(RF_Standalone | RF_Public);
			ExistingClass->GetDefaultObject()->RemoveFromRoot();
			ExistingClass->GetDefaultObject()->ClearFlags(RF_Standalone | RF_Public);
			const FName OldClassRename = MakeUniqueObjectName(GetTransientPackage(), ExistingClass->GetClass(), *FString::Printf(TEXT("HOTRELOADED_%s"), *NameWithoutPrefix));
			ExistingClass->Rename(*OldClassRename.ToString(), GetTransientPackage());
			ExistingClass->SetFlags(RF_Transient);
			ExistingClass->AddToRoot();

			// Make sure enums de-register their names BEFORE we create the new class, otherwise there will be name conflicts
			TArray<UObject*> ClassSubobjects;
			GetObjectsWithOuter(ExistingClass, ClassSubobjects);
			for (auto ClassSubobject : ClassSubobjects)
			{
				if (auto Enum = dynamic_cast<UEnum*>(ClassSubobject))
				{
					Enum->RemoveNamesFromMasterList();
				}
			}
		}
		ClassInfo->OldClass = ExistingClass;
		//向静态数组HotReloadClasses中添加类信息
		GetHotReloadClasses().Add(ClassInfo);

		*ExistingClassInfo = ClassInfo;
	}
	else
	{
		DeferMap.Add(CPPClassName, ClassInfo);
	}
#endif
	// We will either create a new class or update the static class pointer of the existing one
	GetDeferredClassRegistration().Add(ClassInfo);
}

TMap<FName, FDynamicClassStaticData>& GetDynamicClassMap()
{
	static TMap<FName, FDynamicClassStaticData> DynamicClassMap;
	return DynamicClassMap;
}

void UObjectCompiledInDefer(UClass *(*InRegister)(), UClass *(*InStaticClass)(), const TCHAR* Name, const TCHAR* PackageName, bool bDynamic, const TCHAR* DynamicPathName, void (*InInitSearchableValues)(TMap<FName, FName>&))
{
	if (!bDynamic)
	{
#if WITH_HOT_RELOAD
		// Either add all classes if not hot-reloading, or those which have changed
		TMap<FName, FFieldCompiledInInfo*>& DeferMap = GetDeferRegisterClassMap();
		if (!GIsHotReload || DeferMap.FindChecked(Name)->bHasChanged)
#endif
		{
			FString NoPrefix(UObjectBase::RemoveClassPrefix(Name));
			NotifyRegistrationEvent(PackageName, *NoPrefix, ENotifyRegistrationType::NRT_Class, ENotifyRegistrationPhase::NRP_Added, (UObject *(*)())(InRegister), false);
			NotifyRegistrationEvent(PackageName, *(FString(DEFAULT_OBJECT_PREFIX) + NoPrefix), ENotifyRegistrationType::NRT_ClassCDO, ENotifyRegistrationPhase::NRP_Added, (UObject *(*)())(InRegister), false);

			TArray<UClass *(*)()>& DeferredCompiledInRegistration = GetDeferredCompiledInRegistration();
			checkSlow(!DeferredCompiledInRegistration.Contains(InRegister));
			DeferredCompiledInRegistration.Add(InRegister);
		}
	}
	else
	{
		FDynamicClassStaticData ClassFunctions;
		ClassFunctions.ZConstructFn = InRegister;
		ClassFunctions.StaticClassFn = InStaticClass;
		if (InInitSearchableValues)
		{
			InInitSearchableValues(ClassFunctions.SelectedSearchableValues);
		}
		GetDynamicClassMap().Add(FName(DynamicPathName), ClassFunctions);

		FString OriginalPackageName = DynamicPathName;
		check(OriginalPackageName.EndsWith(Name));
		OriginalPackageName.RemoveFromEnd(FString(Name));
		check(OriginalPackageName.EndsWith(TEXT(".")));
		OriginalPackageName.RemoveFromEnd(FString(TEXT(".")));

		NotifyRegistrationEvent(*OriginalPackageName, Name, ENotifyRegistrationType::NRT_Class, ENotifyRegistrationPhase::NRP_Added, (UObject *(*)())(InRegister), true);
		NotifyRegistrationEvent(*OriginalPackageName, *(FString(DEFAULT_OBJECT_PREFIX) + Name), ENotifyRegistrationType::NRT_ClassCDO, ENotifyRegistrationPhase::NRP_Added, (UObject *(*)())(InRegister), true);
	}
}

/** 
* Register all loaded classes 
* 
* 1、GetDeferredClassRegistration()里的元素是静态初始化的时候添加进去的，
*    在XXX.gen.cpp里用static TClassCompiledInDefer这种形式添加。
* 2、TClassCompiledInDefer<TClass>::Register()内部只是简单的转调TClass::StaticClass()。
* 3、TClass::StaticClass()是在XXX.generated.h里的DECLARE_CLASS宏里定义的，
*    内部只是简单的转到GetPrivateStaticClass(TPackage)。
* 4、GetPrivateStaticClass(TPackage)的函数是实现是在IMPLEMENT_CLASS宏里。
*    其内部会真正调用到GetPrivateStaticClassBody。
*    这个函数的内部会创建出UClass对象并调用Register()。
* 5、这里的逻辑是对之前收集到的所有的XXX.gen.cpp里定义的类，
*    都触发一次其UClass的构造，其实也只有UObject比较特殊，会在Static初始化的时候就触发构造。
*    因此这个过程其实是类型系统里每一个类的UClass的创建过程。
* 6、这个函数会被调用多次，在后续的ProcessNewlyLoadedUObjects的里仍然会触发该调用。
*    在FCoreUObjectModule::StartupModule()的这次调用是最先的，
*    这个时候加载编译进来的的类都是引擎启动一开始就链接进来的。
*/
void UClassRegisterAllCompiledInClasses()
{
#if WITH_HOT_RELOAD
	TArray<UClass*> AddedClasses;
#endif
	SCOPED_BOOT_TIMING("UClassRegisterAllCompiledInClasses");
	//GetDeferredClassRegistration()里的元素是之前收集文章里讲的静态初始化的时候添加进去的，
	//在XXX.gen.cpp里用static TClassCompiledInDefer这种形式添加。
	TArray<FFieldCompiledInInfo*>& DeferredClassRegistration = GetDeferredClassRegistration();
	for (const FFieldCompiledInInfo* Class : DeferredClassRegistration)
	{
		//这里的Class其实是TClassCompiledInDefer<TClass>
		//TClassCompiledInDefer<TClass>::Register()内部只是简单的转调TClass::StaticClass()。
		//TClass::StaticClass()是在XXX.generated.h里的DECLARE_CLASS宏里定义的，
		//内部只是简单的转到GetPrivateStaticClass(TPackage)。
		UClass* RegisteredClass = Class->Register();
#if WITH_HOT_RELOAD
		if (GIsHotReload && Class->OldClass == nullptr)
		{
			AddedClasses.Add(RegisteredClass);
		}
#endif
	}
	//前面返回的是引用，因此这里可以清空数据。
	DeferredClassRegistration.Empty();

#if WITH_HOT_RELOAD
	if (AddedClasses.Num() > 0)
	{
		FCoreUObjectDelegates::RegisterHotReloadAddedClassesDelegate.Broadcast(AddedClasses);
	}
#endif
}

#if WITH_HOT_RELOAD
/** Re-instance all existing classes that have changed during hot-reload */
void UClassReplaceHotReloadClasses()
{
	TArray<FFieldCompiledInInfo*>& HotReloadClasses = GetHotReloadClasses();

	if (FCoreUObjectDelegates::RegisterClassForHotReloadReinstancingDelegate.IsBound())
	{
		for (const FFieldCompiledInInfo* Class : HotReloadClasses)
		{
			check(Class->OldClass);

			UClass* RegisteredClass = nullptr;
			if (Class->bHasChanged)
			{
				RegisteredClass = Class->Register();
			}

			FCoreUObjectDelegates::RegisterClassForHotReloadReinstancingDelegate.Broadcast(Class->OldClass, RegisteredClass, Class->bHasChanged ? EHotReloadedClassFlags::Changed : EHotReloadedClassFlags::None);
		}
	}

	FCoreUObjectDelegates::ReinstanceHotReloadedClassesDelegate.Broadcast();
	HotReloadClasses.Empty();
}

#endif

/**
 * Load any outstanding compiled in default properties
 * 针对UClass*对象的构造的重头戏
 */
static void UObjectLoadAllCompiledInDefaultProperties()
{
	TRACE_LOADTIME_REQUEST_GROUP_SCOPE(TEXT("UObjectLoadAllCompiledInDefaultProperties"));
	//引擎包的名字
	static FName LongEnginePackageName(TEXT("/Script/Engine"));
	//从GetDeferredCompiledInRegistration()的源数组里MoveTemp出来遍历。
	TArray<UClass *(*)()>& DeferredCompiledInRegistration = GetDeferredCompiledInRegistration();

	const bool bHaveRegistrants = DeferredCompiledInRegistration.Num() != 0;
	if( bHaveRegistrants )
	{
		SCOPED_BOOT_TIMING("UObjectLoadAllCompiledInDefaultProperties");
		TArray<UClass*> NewClasses;
		TArray<UClass*> NewClassesInCoreUObject;
		TArray<UClass*> NewClassesInEngine;
		TArray<UClass* (*)()> PendingRegistrants = MoveTemp(DeferredCompiledInRegistration);
		for (UClass* (*Registrant)() : PendingRegistrants)
		{
			//调用生成代码里的Z_Construct_UClass_UMyClass创建UClass*
			UClass* Class = Registrant();
			UE_LOG(LogUObjectBootstrap, Verbose, TEXT("UObjectLoadAllCompiledInDefaultProperties After Registrant %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
			//按照所属于的Package分到3个数组里
			if (Class->GetOutermost()->GetFName() == GLongCoreUObjectPackageName)
			{
				NewClassesInCoreUObject.Add(Class);
			}
			else if (Class->GetOutermost()->GetFName() == LongEnginePackageName)
			{
				NewClassesInEngine.Add(Class);
			}
			else
			{
				NewClasses.Add(Class);
			}
		}
		{
			//CoreUObject数组内的对象依次构造CDO对象
			SCOPED_BOOT_TIMING("CoreUObject Classes");
			for (UClass* Class : NewClassesInCoreUObject) // we do these first because we assume these never trigger loads
			{
				UE_LOG(LogUObjectBootstrap, Verbose, TEXT("GetDefaultObject Begin %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
				Class->GetDefaultObject();
				UE_LOG(LogUObjectBootstrap, Verbose, TEXT("GetDefaultObject End %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
			}
		}
		{
			//Engine数组内的对象依次构造CDO对象
			SCOPED_BOOT_TIMING("Engine Classes");
			for (UClass* Class : NewClassesInEngine) // we do these second because we want to bring the engine up before the game
			{
				UE_LOG(LogUObjectBootstrap, Verbose, TEXT("GetDefaultObject Begin %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
				Class->GetDefaultObject();
				UE_LOG(LogUObjectBootstrap, Verbose, TEXT("GetDefaultObject End %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
			}
		}
		{
			//Other数组内的对象依次构造CDO对象
			SCOPED_BOOT_TIMING("Other Classes");
			for (UClass* Class : NewClasses)
			{
				UE_LOG(LogUObjectBootstrap, Verbose, TEXT("GetDefaultObject Begin %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
				Class->GetDefaultObject();
				UE_LOG(LogUObjectBootstrap, Verbose, TEXT("GetDefaultObject End %s %s"), *Class->GetOutermost()->GetName(), *Class->GetName());
			}
		}
		FFeedbackContext& ErrorsFC = UClass::GetDefaultPropertiesFeedbackContext();
		if (ErrorsFC.GetNumErrors() || ErrorsFC.GetNumWarnings())
		{
			TArray<FString> AllErrorsAndWarnings;
			ErrorsFC.GetErrorsAndWarningsAndEmpty(AllErrorsAndWarnings);

			FString AllInOne;
			UE_LOG(LogUObjectBase, Warning, TEXT("-------------- Default Property warnings and errors:"));
			for (const FString& ErrorOrWarning : AllErrorsAndWarnings)
			{
				UE_LOG(LogUObjectBase, Warning, TEXT("%s"), *ErrorOrWarning);
				AllInOne += ErrorOrWarning;
				AllInOne += TEXT("\n");
			}
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format( NSLOCTEXT("Core", "DefaultPropertyWarningAndErrors", "Default Property warnings and errors:\n{0}"), FText::FromString( AllInOne ) ) );
		}
	}
}

/**
 * Call StaticStruct for each struct...this sets up the internal singleton, and important works correctly with hot reload
 * 这一步开始真正的构造UEnum和UScriptStruct
 */
static void UObjectLoadAllCompiledInStructs()
{
	SCOPED_BOOT_TIMING("UObjectLoadAllCompiledInStructs");

	//1、先创建EnumRegistrant.PackageName再创建StructRegistrant.PackageName。
	//   这两个名字值都是在生成代码里定义的，同UClass一样，表示了其所在的Package。	
	//2、MoveTemp会触发TArray的右移引用赋值，把源数组里的数据迁移到目标数组里去。
	//   所以外层的while判断值才会改变。
	TArray<FPendingEnumRegistrant> PendingEnumRegistrants = MoveTemp(GetDeferredCompiledInEnumRegistration());
	TArray<FPendingStructRegistrant> PendingStructRegistrants = MoveTemp(GetDeferredCompiledInStructRegistration());

	{
		SCOPED_BOOT_TIMING("UObjectLoadAllCompiledInStructs -  CreatePackages (could be optimized!)");
		// Load Enums first
		for (const FPendingEnumRegistrant& EnumRegistrant : PendingEnumRegistrants)
		{
			// Make sure the package exists in case it does not contain any UObjects
			//创建其所属于的Package
			//CreatePackage的里面总是会先查找该名字的Package是否已经存在，不会重复创建。
			CreatePackage(EnumRegistrant.PackageName);
		}
		for (const FPendingStructRegistrant& StructRegistrant : PendingStructRegistrants)
		{
			// Make sure the package exists in case it does not contain any UObjects or UEnums
			//创建其所属于的Package
			CreatePackage(StructRegistrant.PackageName);
		}
	}

	// Load Structs
	//先enum再struct的调用其注册函数RegisterFn()。
	//RegisterFn是个函数指针，指向生成代码里Z_Construct开头的函数，用来真正构造出UEnum和UScriptStruct对象。
	//有意思的是，顺序总是先enum再struct，因为更基础的类型总是先构造。
	for (const FPendingEnumRegistrant& EnumRegistrant : PendingEnumRegistrants)
	{
		//调用生成代码里Z_Construct_UEnum_Hello_EMyEnum
		EnumRegistrant.RegisterFn();
	}

	for (const FPendingStructRegistrant& StructRegistrant : PendingStructRegistrants)
	{
		//调用生成代码里Z_Construct_UScriptStruct_FMyStruct
		StructRegistrant.RegisterFn();
	}
}

/**
* 该函数会在模块加载后再次触发调用，对于该函数的理解，
* 1、它是重复调用多次的，
* 2、它的内部流程是一个完整的流程。
*/
void ProcessNewlyLoadedUObjects(FName Package, bool bCanProcessNewlyLoadedObjects)
{
	SCOPED_BOOT_TIMING("ProcessNewlyLoadedUObjects");
#if USE_PER_MODULE_UOBJECT_BOOTSTRAP
	if (Package != NAME_None)
	{
		UObjectReleaseModuleRegistrants(Package);
	}
#endif
	if (!bCanProcessNewlyLoadedObjects)
	{
		return;
	}
	LLM_SCOPE(ELLMTag::UObject);
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("ProcessNewlyLoadedUObjects"), STAT_ProcessNewlyLoadedUObjects, STATGROUP_ObjectVerbose);
	
	//【为代码里定义的那些类生成UClass*】
	//主要是为每一个编译进来的class调用TClass::StaticClass()来构造出UClass* 对象。
	UClassRegisterAllCompiledInClasses();

	//【提取收集到的注册项信息】
	//GetDeferredCompiledInRegistration()是之前信息收集的时候static FCompiledInDefer变量初始化时收集到的全局数组
	//和定义的class一对一
	const TArray<UClass* (*)()>& DeferredCompiledInRegistration = GetDeferredCompiledInRegistration();
	//GetDeferredCompiledInStructRegistration()是之前信息收集的时候static FCompiledInDeferStruct变量初始化时收集到的全局数组，
	//和定义的struct一对一
	const TArray<FPendingStructRegistrant>& DeferredCompiledInStructRegistration = GetDeferredCompiledInStructRegistration();
	//GetDeferredCompiledInEnumRegistration()是之前信息收集的时候static FCompiledInDeferEnum变量初始化时收集到的全局数组，
	//和定义的enum一对一
	const TArray<FPendingEnumRegistrant>& DeferredCompiledInEnumRegistration = GetDeferredCompiledInEnumRegistration();

	//有待注册项就继续循环注册
	bool bNewUObjects = false;
	while (GFirstPendingRegistrant || DeferredCompiledInRegistration.Num() || DeferredCompiledInStructRegistration.Num() || DeferredCompiledInEnumRegistration.Num())
	{
		bNewUObjects = true;
		//为之前生成的UClass*注册，生成其Package。
		//这里调用的目的是在后续的操作之前确保内存里已经把相关的类型UClass*对象都已经注册完毕
		UObjectProcessRegistrants();
		//为代码里的枚举和结构构造类型对象，分别生成UEnum和UScriptStruct
		UObjectLoadAllCompiledInStructs();
		//为UClass*们继续构造和创建类默认对象(CDO)
		UObjectLoadAllCompiledInDefaultProperties();
	}
#if WITH_HOT_RELOAD
	UClassReplaceHotReloadClasses();
#endif

	//最后一步判断如果有新UClass* 对象生成了，
	//并且现在不在初始化载入阶段（GIsInitialLoad初始=true，只有在后续开启GC后才=false表示初始化载入过程结束了）
	if (bNewUObjects && !GIsInitialLoad)
	{
		//【构造引用记号流，为后续GC用】
	    //用AssembleReferenceTokenStreams为UClass创建引用记号流（一种辅助GC分析对象引用的数据结构）
		//所以第一次的FEngineLoop::PreInit()里的ProcessNewlyLoadedUObjects并不会触发
		//AssembleReferenceTokenStreams的调用但也会在后续的GUObjectArray.CloseDisregardForGC()里面
		//调用AssembleReferenceTokenStreams。
		//只有后续模块动态加载后触发的ProcessNewlyLoadedUObjects才会AssembleReferenceTokenStreams。
		//通过这个判断保证了在两种情况下，AssembleReferenceTokenStreams只会被调用一次。
		UClass::AssembleReferenceTokenStreams();
	}
}

static int32 GVarMaxObjectsNotConsideredByGC;
static FAutoConsoleVariableRef CMaxObjectsNotConsideredByGC(
	TEXT("gc.MaxObjectsNotConsideredByGC"),
	GVarMaxObjectsNotConsideredByGC,
	TEXT("Placeholder console variable, currently not used in runtime."),
	ECVF_Default
	);

static int32 GSizeOfPermanentObjectPool;
static FAutoConsoleVariableRef CSizeOfPermanentObjectPool(
	TEXT("gc.SizeOfPermanentObjectPool"),
	GSizeOfPermanentObjectPool,
	TEXT("Placeholder console variable, currently not used in runtime."),
	ECVF_Default
	);

static int32 GMaxObjectsInEditor;
static FAutoConsoleVariableRef CMaxObjectsInEditor(
	TEXT("gc.MaxObjectsInEditor"),
	GMaxObjectsInEditor,
	TEXT("Placeholder console variable, currently not used in runtime."),
	ECVF_Default
	);

static int32 GMaxObjectsInGame;
static FAutoConsoleVariableRef CMaxObjectsInGame(
	TEXT("gc.MaxObjectsInGame"),
	GMaxObjectsInGame,
	TEXT("Placeholder console variable, currently not used in runtime."),
	ECVF_Default
	);


/**
 * Final phase of UObject initialization. 
 * all auto register objects are added to the main data structures.
 * 
 * 这个函数主要做了4件事： 
 * 1. 初始化UObject的内存分配存储系统和对象的Hash系统。 
 * 2. 创建了异步加载线程，用来后续Package(uasset)的加载。 
 * 3. GObjInitialized=true，这样在后续就可以用bool UObjectInitialized()来判断对象系统是否可用。 
 * 4. 继续转发到UObjectProcessRegistrants来把注册项一一处理。
 */
void UObjectBaseInit()
{
	SCOPED_BOOT_TIMING("UObjectBaseInit");

	// Zero initialize and later on get value from .ini so it is overridable per game/ platform...
	int32 MaxObjectsNotConsideredByGC = 0;
	int32 SizeOfPermanentObjectPool = 0;
	int32 MaxUObjects = 2 * 1024 * 1024; // Default to ~2M UObjects
	bool bPreAllocateUObjectArray = false;	

	// To properly set MaxObjectsNotConsideredByGC look for "Log: XXX objects as part of root set at end of initial load."
	// in your log file. This is being logged from LaunchEnglineLoop after objects have been added to the root set. 

	// Disregard for GC relies on seekfree loading for interaction with linkers. We also don't want to use it in the Editor, for which
	// FPlatformProperties::RequiresCookedData() will be false. Please note that GIsEditor and FApp::IsGame() are not valid at this point.
	if (FPlatformProperties::RequiresCookedData())
	{
		FString Value;
		bool bIsCookOnTheFly = FParse::Value(FCommandLine::Get(), TEXT("-filehostip="), Value);
		if (bIsCookOnTheFly)
		{
			GCreateGCClusters = false;
		}
		else
		{
			GConfig->GetInt(TEXT("/Script/Engine.GarbageCollectionSettings"), TEXT("gc.MaxObjectsNotConsideredByGC"), MaxObjectsNotConsideredByGC, GEngineIni);

			// Not used on PC as in-place creation inside bigger pool interacts with the exit purge and deleting UObject directly.
			GConfig->GetInt(TEXT("/Script/Engine.GarbageCollectionSettings"), TEXT("gc.SizeOfPermanentObjectPool"), SizeOfPermanentObjectPool, GEngineIni);
		}

		// Maximum number of UObjects in cooked game
		GConfig->GetInt(TEXT("/Script/Engine.GarbageCollectionSettings"), TEXT("gc.MaxObjectsInGame"), MaxUObjects, GEngineIni);

		// If true, the UObjectArray will pre-allocate all entries for UObject pointers
		GConfig->GetBool(TEXT("/Script/Engine.GarbageCollectionSettings"), TEXT("gc.PreAllocateUObjectArray"), bPreAllocateUObjectArray, GEngineIni);
	}
	else
	{
#if IS_PROGRAM
		// Maximum number of UObjects for programs can be low
		MaxUObjects = 100000; // Default to 100K for programs
		GConfig->GetInt(TEXT("/Script/Engine.GarbageCollectionSettings"), TEXT("gc.MaxObjectsInProgram"), MaxUObjects, GEngineIni);
#else
		// Maximum number of UObjects in the editor
		GConfig->GetInt(TEXT("/Script/Engine.GarbageCollectionSettings"), TEXT("gc.MaxObjectsInEditor"), MaxUObjects, GEngineIni);
#endif
	}

	if (MaxObjectsNotConsideredByGC <= 0 && SizeOfPermanentObjectPool > 0)
	{
		// If permanent object pool is enabled but disregard for GC is disabled, GC will mark permanent object pool objects
		// as unreachable and may destroy them so disable permanent object pool too.
		// An alternative would be to make GC not mark permanent object pool objects as unreachable but then they would have to
		// be considered as root set objects because they could be referencing objects from outside of permanent object pool.
		// This would be inconsistent and confusing and also counter productive (the more root set objects the more expensive MarkAsUnreachable phase is).
		SizeOfPermanentObjectPool = 0;
		UE_LOG(LogInit, Warning, TEXT("Disabling permanent object pool because disregard for GC is disabled (gc.MaxObjectsNotConsideredByGC=%d)."), MaxObjectsNotConsideredByGC);
	}

	// Log what we're doing to track down what really happens as log in LaunchEngineLoop doesn't report those settings in pristine form.
	UE_LOG(LogInit, Log, TEXT("%s for max %d objects, including %i objects not considered by GC, pre-allocating %i bytes for permanent pool."), 
		bPreAllocateUObjectArray ? TEXT("Pre-allocating") : TEXT("Presizing"),
		MaxUObjects, MaxObjectsNotConsideredByGC, SizeOfPermanentObjectPool);
	//初始化对象分配器
	GUObjectAllocator.AllocatePermanentObjectPool(SizeOfPermanentObjectPool);
	//初始化对象管理数组
	GUObjectArray.AllocateObjectPool(MaxUObjects, MaxObjectsNotConsideredByGC, bPreAllocateUObjectArray);

	void InitAsyncThread();
	//初始化Package(uasset)的异步加载线程
	InitAsyncThread();

	// Note initialized.
	//指定UObject系统初始化完毕
	Internal::GetUObjectSubsystemInitialised() = true;
	
	//处理注册项
	UObjectProcessRegistrants();
}

/**
 * Final phase of UObject shutdown
 */
void UObjectBaseShutdown()
{
	void ShutdownAsyncThread();
	ShutdownAsyncThread();

	GUObjectArray.ShutdownUObjectArray();
	Internal::GetUObjectSubsystemInitialised() = false;
}

/**
 * Helper function that can be used inside the debuggers watch window. E.g. "DebugFName(Class)". 
 *
 * @param	Object	Object to look up the name for 
 * @return			Associated name
 */
const TCHAR* DebugFName(UObject* Object)
{
	if ( Object )
	{
		// Hardcoded static array. This function is only used inside the debugger so it should be fine to return it.
		static TCHAR TempName[256];
		FName Name = Object->GetFName();
		FCString::Strcpy(TempName, *FName::SafeString(Name.GetDisplayIndex(), Name.GetNumber()));
		return TempName;
	}
	else
	{
		return TEXT("NULL");
	}
}

/**
 * Helper function that can be used inside the debuggers watch window. E.g. "DebugFName(Object)". 
 *
 * @param	Object	Object to look up the name for 
 * @return			Fully qualified path name
 */
const TCHAR* DebugPathName(UObject* Object)
{
	if( Object )
	{
		// Hardcoded static array. This function is only used inside the debugger so it should be fine to return it.
		static TCHAR PathName[1024];
		PathName[0] = 0;

		// Keep track of how many outers we have as we need to print them in inverse order.
		UObject*	TempObject = Object;
		int32			OuterCount = 0;
		while( TempObject )
		{
			TempObject = TempObject->GetOuter();
			OuterCount++;
		}

		// Iterate over each outer + self in reverse oder and append name.
		for( int32 OuterIndex=OuterCount-1; OuterIndex>=0; OuterIndex-- )
		{
			// Move to outer name.
			TempObject = Object;
			for( int32 i=0; i<OuterIndex; i++ )
			{
				TempObject = TempObject->GetOuter();
			}

			// Dot separate entries.
			if( OuterIndex != OuterCount -1 )
			{
				FCString::Strcat( PathName, TEXT(".") );
			}
			// And app end the name.
			FCString::Strcat( PathName, DebugFName( TempObject ) );
		}

		return PathName;
	}
	else
	{
		return TEXT("None");
	}
}

/**
 * Helper function that can be used inside the debuggers watch window. E.g. "DebugFName(Object)". 
 *
 * @param	Object	Object to look up the name for 
 * @return			Fully qualified path name prepended by class name
 */
const TCHAR* DebugFullName(UObject* Object)
{
	if( Object )
	{
		// Hardcoded static array. This function is only used inside the debugger so it should be fine to return it.
		static TCHAR FullName[1024];
		FullName[0]=0;

		// Class Full.Path.Name
		FCString::Strcat( FullName, DebugFName(Object->GetClass()) );
		FCString::Strcat( FullName, TEXT(" "));
		FCString::Strcat( FullName, DebugPathName(Object) );

		return FullName;
	}
	else
	{
		return TEXT("None");
	}
}

#if WITH_HOT_RELOAD
namespace
{
	struct FObjectCompiledInfo
	{
		/** Registered struct info (including size and reflection info) */
		static TMap<TTuple<UObject*, FName>, FObjectCompiledInfo>& GetRegisteredInfo()
		{
			static TMap<TTuple<UObject*, FName>, FObjectCompiledInfo> StructOrEnumCompiledInfoMap;
			return StructOrEnumCompiledInfoMap;
		}

		FObjectCompiledInfo(SIZE_T InClassSize, uint32 InCrc)
			: Size(InClassSize)
			, Crc (InCrc)
		{
		}

		SIZE_T Size;
		uint32 Crc;
	};

	template <typename TType>
	TType* FindExistingObjectIfHotReload(UObject* Outer, const TCHAR* Name, SIZE_T Size, uint32 Crc)
	{
		TTuple<UObject*, FName> Key(Outer, Name);

		bool bChanged = true;
		if (FObjectCompiledInfo* Info = FObjectCompiledInfo::GetRegisteredInfo().Find(Key))
		{
			// Hot-reloaded struct
			bChanged = Info->Size != Size || Info->Crc != Crc;

			Info->Size = Size;
			Info->Crc  = Crc;
		}
		else
		{
			// New struct
			FObjectCompiledInfo::GetRegisteredInfo().Add(Key, FObjectCompiledInfo(Size, Crc));
		}

		if (!GIsHotReload)
		{
			return nullptr;
		}

		TType* Existing = FindObject<TType>(Outer, Name);
		if (!Existing)
		{
			// New type added during hot-reload
			UE_LOG(LogClass, Log, TEXT("Could not find existing type %s for HotReload. Assuming new"), Name);
			return nullptr;
		}

		// Existing type, make sure we destroy the old one if it has changed
		if (bChanged)
		{
			// Make sure the old struct is not used by anything
			Existing->ClearFlags(RF_Standalone | RF_Public);
			Existing->RemoveFromRoot();
			const FName OldRename = MakeUniqueObjectName(GetTransientPackage(), Existing->GetClass(), *FString::Printf(TEXT("HOTRELOADED_%s"), Name));
			Existing->Rename(*OldRename.ToString(), GetTransientPackage());
			return nullptr;
		}

		UE_LOG(LogClass, Log, TEXT("%s HotReload."), Name);
		return Existing;
	}
}
#endif // WITH_HOT_RELOAD

UScriptStruct* FindExistingStructIfHotReloadOrDynamic(UObject* Outer, const TCHAR* StructName, SIZE_T Size, uint32 Crc, bool bIsDynamic)
{
#if WITH_HOT_RELOAD
	UScriptStruct* Result = FindExistingObjectIfHotReload<UScriptStruct>(Outer, StructName, Size, Crc);
#else
	UScriptStruct* Result = nullptr;
#endif
	if (!Result && bIsDynamic)
	{
		Result = Cast<UScriptStruct>(StaticFindObjectFast(UScriptStruct::StaticClass(), Outer, StructName));
	}
	return Result;
}

UEnum* FindExistingEnumIfHotReloadOrDynamic(UObject* Outer, const TCHAR* EnumName, SIZE_T Size, uint32 Crc, bool bIsDynamic)
{
#if WITH_HOT_RELOAD
	UEnum* Result = FindExistingObjectIfHotReload<UEnum>(Outer, EnumName, Size, Crc);
#else
	UEnum* Result = nullptr;
#endif
	if (!Result && bIsDynamic)
	{
		Result = Cast<UEnum>(StaticFindObjectFast(UEnum::StaticClass(), Outer, EnumName));
	}
	return Result;
}

UObject* ConstructDynamicType(FName TypePathName, EConstructDynamicType ConstructionSpecifier)
{
	UObject* Result = nullptr;
	if (FDynamicClassStaticData* ClassConstructFn = GetDynamicClassMap().Find(TypePathName))
	{
		if (ConstructionSpecifier == EConstructDynamicType::CallZConstructor)
		{
			UClass* DynamicClass = ClassConstructFn->ZConstructFn();
			check(DynamicClass);
			DynamicClass->AssembleReferenceTokenStream();
			Result = DynamicClass;
		}
		else if (ConstructionSpecifier == EConstructDynamicType::OnlyAllocateClassObject)
		{
			Result = ClassConstructFn->StaticClassFn();
			check(Result);
		}
	}
	else if (UScriptStruct *(**StaticStructFNPtr)() = GetDynamicStructMap().Find(TypePathName))
	{
		Result = (*StaticStructFNPtr)();
	}
	else if (UEnum *(**StaticEnumFNPtr)() = GetDynamicEnumMap().Find(TypePathName))
	{
		Result = (*StaticEnumFNPtr)();
	}
	return Result;
}

FName GetDynamicTypeClassName(FName TypePathName)
{
	FName Result = NAME_None;
	if (GetDynamicClassMap().Find(TypePathName))
	{
		Result = UDynamicClass::StaticClass()->GetFName();
	}
	else if (GetDynamicStructMap().Find(TypePathName))
	{
		Result = UScriptStruct::StaticClass()->GetFName();
	}
	else if (GetDynamicEnumMap().Find(TypePathName))
	{
		Result = UEnum::StaticClass()->GetFName();
	}
	if (false && Result == NAME_None)
	{
		UE_LOG(LogUObjectBase, Warning, TEXT("GetDynamicTypeClassName %s not found."), *TypePathName.ToString());
		UE_LOG(LogUObjectBase, Warning, TEXT("---- classes"));
		for (auto& Pair : GetDynamicClassMap())
		{
			UE_LOG(LogUObjectBase, Warning, TEXT("    %s"), *Pair.Key.ToString());
		}
		UE_LOG(LogUObjectBase, Warning, TEXT("---- structs"));
		for (auto& Pair : GetDynamicStructMap())
		{
			UE_LOG(LogUObjectBase, Warning, TEXT("    %s"), *Pair.Key.ToString());
		}
		UE_LOG(LogUObjectBase, Warning, TEXT("---- enums"));
		for (auto& Pair : GetDynamicEnumMap())
		{
			UE_LOG(LogUObjectBase, Warning, TEXT("    %s"), *Pair.Key.ToString());
		}
		UE_LOG(LogUObjectBase, Fatal, TEXT("GetDynamicTypeClassName %s not found."), *TypePathName.ToString());
	}
	UE_CLOG(Result == NAME_None, LogUObjectBase, Warning, TEXT("GetDynamicTypeClassName %s not found."), *TypePathName.ToString());
	return Result;
}

UPackage* FindOrConstructDynamicTypePackage(const TCHAR* PackageName)
{
	UPackage* Package = Cast<UPackage>(StaticFindObjectFast(UPackage::StaticClass(), nullptr, PackageName));
	if (!Package)
	{
		Package = CreatePackage(PackageName);
		if (!GEventDrivenLoaderEnabled)
		{
			Package->SetPackageFlags(PKG_CompiledIn);
		}
	}
	check(Package);
	return Package;
}

TMap<FName, FName>& GetConvertedDynamicPackageNameToTypeName()
{
	static TMap<FName, FName> ConvertedDynamicPackageNameToTypeName;
	return ConvertedDynamicPackageNameToTypeName;
}
