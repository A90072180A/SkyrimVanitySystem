#pragma once

// Minimal ABI-compatible subset of RaceMenu/skee's public ModderResource
// IPluginInterface.h. The engine classes remain opaque global-namespace
// pointers exactly as skee exposes them; the adapter bridges from RE::* with
// reinterpret_cast only at the API boundary.

class TESObjectREFR;
class TESObjectARMO;
class TESObjectARMA;
class NiAVObject;
class NiNode;
class BSGeometry;
class NiSkinPartition;
class NiBinaryExtraData;

using skee_u64 = std::uint64_t;
using skee_u32 = std::uint32_t;

class IPluginInterface
{
public:
    virtual ~IPluginInterface() = default;
    virtual skee_u32 GetVersion() = 0;
    virtual void Revert() = 0;
};

class IInterfaceMap
{
public:
    virtual IPluginInterface* QueryInterface(const char* a_name) = 0;
    virtual bool AddInterface(const char* a_name, IPluginInterface* a_interface) = 0;
    virtual IPluginInterface* RemoveInterface(const char* a_name) = 0;
};

struct InterfaceExchangeMessage
{
    enum : std::uint32_t { kMessage_ExchangeInterface = 0x9E3779B9u };
    IInterfaceMap* interfaceMap{nullptr};
};

class IAddonAttachmentInterface
{
public:
    virtual ~IAddonAttachmentInterface() = default;
    virtual void OnAttach(
        TESObjectREFR* a_refr,
        TESObjectARMO* a_armor,
        TESObjectARMA* a_addon,
        NiAVObject* a_object,
        bool a_isFirstPerson,
        NiNode* a_skeleton,
        NiNode* a_root) = 0;
};

class IBodyMorphInterface : public IPluginInterface
{
public:
    class MorphKeyVisitor
    {
    public:
        virtual ~MorphKeyVisitor() = default;
        virtual void Visit(const char*, float) = 0;
    };

    class StringVisitor
    {
    public:
        virtual ~StringVisitor() = default;
        virtual void Visit(const char*) = 0;
    };

    class ActorVisitor
    {
    public:
        virtual ~ActorVisitor() = default;
        virtual void Visit(TESObjectREFR*) = 0;
    };

    class MorphValueVisitor
    {
    public:
        virtual ~MorphValueVisitor() = default;
        virtual void Visit(TESObjectREFR*, const char*, const char*, float) = 0;
    };

    class MorphVisitor
    {
    public:
        virtual ~MorphVisitor() = default;
        virtual void Visit(TESObjectREFR*, const char*) = 0;
    };

    virtual void SetMorph(TESObjectREFR*, const char*, const char*, float) = 0;
    virtual float GetMorph(TESObjectREFR*, const char*, const char*) = 0;
    virtual void ClearMorph(TESObjectREFR*, const char*, const char*) = 0;
    virtual float GetBodyMorphs(TESObjectREFR*, const char*) = 0;
    virtual void ClearBodyMorphNames(TESObjectREFR*, const char*) = 0;
    virtual void VisitMorphs(TESObjectREFR*, MorphVisitor&) = 0;
    virtual void VisitKeys(TESObjectREFR*, const char*, MorphKeyVisitor&) = 0;
    virtual void VisitMorphValues(TESObjectREFR*, MorphValueVisitor&) = 0;
    virtual void ClearMorphs(TESObjectREFR*) = 0;
    virtual void ApplyVertexDiff(TESObjectREFR*, NiAVObject*, bool a_attach = false) = 0;
    virtual void ApplyBodyMorphs(TESObjectREFR*, bool a_deferUpdate = true) = 0;
    virtual void UpdateModelWeight(TESObjectREFR*, bool a_immediate = false) = 0;
    virtual void SetCacheLimit(skee_u64) = 0;
    virtual bool HasMorphs(TESObjectREFR*) = 0;
    virtual skee_u32 EvaluateBodyMorphs(TESObjectREFR*) = 0;
    virtual bool HasBodyMorph(TESObjectREFR*, const char*, const char*) = 0;
    virtual bool HasBodyMorphName(TESObjectREFR*, const char*) = 0;
    virtual bool HasBodyMorphKey(TESObjectREFR*, const char*) = 0;
    virtual void ClearBodyMorphKeys(TESObjectREFR*, const char*) = 0;
    virtual void VisitStrings(StringVisitor&) = 0;
    virtual void VisitActors(ActorVisitor&) = 0;
    virtual skee_u64 ClearMorphCache() = 0;

    using MorphShapeCallback = void (*)(
        TESObjectREFR*, NiAVObject*, BSGeometry*, NiSkinPartition*, NiBinaryExtraData*);
    virtual void AddMorphShapeCallback(MorphShapeCallback, skee_u64 a_order = 0) = 0;
};

class IActorUpdateManager : public IPluginInterface
{
public:
    virtual void AddBodyUpdate(skee_u32) = 0;
    virtual void AddTransformUpdate(skee_u32) = 0;
    virtual void AddOverlayUpdate(skee_u32) = 0;
    virtual void AddNodeOverrideUpdate(skee_u32) = 0;
    virtual void AddWeaponOverrideUpdate(skee_u32) = 0;
    virtual void AddAddonOverrideUpdate(skee_u32) = 0;
    virtual void AddSkinOverrideUpdate(skee_u32) = 0;
    virtual void Flush() = 0;
    virtual void AddInterface(IAddonAttachmentInterface*) = 0;
    virtual void RemoveInterface(IAddonAttachmentInterface*) = 0;

    using FlushCallback = void (*)(skee_u32*, skee_u32);
    virtual bool RegisterFlushCallback(const char*, FlushCallback) = 0;
    virtual bool UnregisterFlushCallback(const char*) = 0;
};
