#include "Plugin.h"
#include "RaceMenuBridge.h"
#include "FootGeometryCapture.h"
#include "HeightProfiles.h"
#include "BarefootPolicy.h"
#include "FootCapturePolicy.h"
#include "api/SkyrimVanitySystemAPI.h"
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <map>
#include <set>
#include <thread>
#include <tuple>

namespace vanity_ube_heel_adapter {
namespace {
using Json=nlohmann::json;
namespace api=SkyrimVanitySystemAPI;
namespace hp=height_plan_core;
constexpr auto kConfig="Data/SKSE/Plugins/VanityUBEHeelAdapter.json";
api::ISkyrimVanitySystemInterface001* svs{};
api::ListenerHandle listener{};
std::atomic_bool queued{false},running{false};
std::atomic<std::uint64_t> dirty{1};
Json config=Json::object();
std::mutex updateMutex;
bool loggedUnavailable=false,registered=false;
struct Application {RE::NiPointer<RE::NiAVObject> object;std::string desired,stamp;std::uint64_t dirty{};};
std::map<std::uintptr_t,Application> applications;
barefoot_policy::Settler barefootSettler;
std::map<std::string,std::string> lastDecision;
void DecisionLog(const std::string& key,const std::string& message){
    auto it=lastDecision.find(key);
    if(it!=lastDecision.end()&&it->second==message)return;
    if(lastDecision.size()>=128)lastDecision.clear();
    lastDecision[key]=message;
    logger::info("[height decision] {}",message);
}
void QueueSnapshot();
void Changed(){dirty.fetch_add(1);QueueSnapshot();}
std::string Stable(RE::FormID id){
    auto* form=RE::TESForm::LookupByID(id);auto* file=form?form->GetFile(0):nullptr;
    return file?std::format("{}|{:08X}",file->GetFilename(),form->GetLocalFormID()):std::string{};
}
std::string Stem(std::string path){
    path=foot_capture_policy::ResourceKey(path);if(path.empty())return {};
    auto dot=path.find_last_of('.');if(dot!=std::string::npos)path.erase(dot);
    if(path.ends_with("_0")||path.ends_with("_1"))path.resize(path.size()-2);
    return path;
}
bool Token(std::string text,std::initializer_list<std::string_view> words){
    std::ranges::transform(text,text.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
    for(auto word:words)if(text.find(word)!=std::string::npos)return true;return false;
}
bool Bool(const char*name,bool fallback){auto it=config.find(name);return it!=config.end()&&it->is_boolean()?it->get<bool>():fallback;}
struct Visual {
    RE::FormID armor{},addon{};std::string armorID,addonID,model;
    std::uint64_t slots{};bool stocking{};
};
struct Live {
    Visual visual;
    RE::NiPointer<RE::NiAVObject> root;
    std::string tri;
};
bool ExplicitStocking(const std::string& id){
    auto it=config.find("stockings");if(it==config.end()||!it->is_array())return false;
    return std::ranges::any_of(*it,[&](const Json&j){return j.is_string()&&j.get<std::string>()==id;});
}
bool IsStocking(const Visual&v){
    if(ExplicitStocking(v.armorID))return true;
    if(!Bool("autoDetectStockings",true))return false;
    // A slot on a visual, not the real equipped trigger, supplies supporting
    // evidence. Slot 32/37 triggers never turn a stocking into a shoe.
    constexpr auto hosiery=(1ULL<<(38-30))|(1ULL<<(48-30))|(1ULL<<(53-30));
    return Token(v.model,{"stocking","pantyhose","tights","hosiery","thighhigh"})||
        ((v.slots&hosiery)&&!Token(v.model,{"shoe","boot","heel","glove"}));
}
std::vector<Visual> Visuals(const api::VisualState001& state){
    std::vector<Visual> out;std::set<std::tuple<RE::FormID,RE::FormID,std::string>> seen;
    if(state.interfaceRevision!=api::kInterfaceRevision001||state.structureSize<sizeof(state)||
       state.pieceStructureSize!=sizeof(api::VisualPiece001)||state.pieceCount>4096||
       (state.pieceCount&&!state.pieces))return out;
    for(unsigned i=0;i<state.pieceCount;++i){const auto&p=state.pieces[i];
        if(p.structureSize<sizeof(p)||!p.replacementArmorFormID||!p.replacementArmorAddonFormID||
           (p.flags&api::kVisualPiece_Hidden)||!p.actorModelPath)continue;
        Visual v{p.replacementArmorFormID,p.replacementArmorAddonFormID,Stable(p.replacementArmorFormID),
            Stable(p.replacementArmorAddonFormID),p.actorModelPath,p.visualSlotMask,false};
        v.stocking=IsStocking(v);
        if((v.stocking||(v.slots&(1ULL<<7)))&&seen.emplace(v.armor,v.addon,v.model).second)out.push_back(std::move(v));
    }
    return out;
}
std::vector<Live> MatchLive(const std::vector<Visual>& visuals,const std::vector<racemenu::BipedPartRecord>&parts){
    std::vector<Live> out;
    for(const auto&v:visuals){std::map<std::uintptr_t,Live> matches;
        const auto stem=Stem(v.model);if(stem.empty())continue;
        for(const auto&p:parts){if(p.buffered||!p.partClone)continue;
            // Actual BODYTRI stem is required even if a stale/synthetic ARMA ID
            // happens to match. Buffered parts and attachment-cache-only nodes
            // are never authority. Parent slots may differ from the visual slot.
            RE::BSVisit::TraverseScenegraphObjects(p.partClone.get(),[&](RE::NiAVObject*n){
                auto* extra=n->GetExtraData<RE::NiStringExtraData>("BODYTRI");
                if(extra&&extra->value&&Stem(extra->value)==stem){
                    const auto id=reinterpret_cast<std::uintptr_t>(n);
                    matches.try_emplace(id,Live{v,RE::NiPointer<RE::NiAVObject>(n),extra->value});
                }return RE::BSVisit::BSVisitControl::kContinue;
            });
        }
        // Duplicate BODYTRI on a root and its descendant is one attachment,
        // not two competing outfits. Keep the outermost scoped marker.
        std::vector<std::uintptr_t> nested;
        for(const auto&[id,value]:matches) {
            auto* node=value.root->parent;
            for(unsigned depth=0;node&&depth<64;++depth,node=node->parent)
                if(matches.contains(reinterpret_cast<std::uintptr_t>(node))){nested.push_back(id);break;}
        }
        for(auto id:nested)matches.erase(id);
        if(matches.size()==1)out.push_back(std::move(matches.begin()->second));
        else if(matches.size()>1)logger::warn("[height target] ambiguous live BODYTRI objects for {}",v.armorID);
    }
    return out;
}
std::string Stamp(RE::NiAVObject*root){
    std::string out;
    RE::BSVisit::TraverseScenegraphGeometries(root,[&](RE::BSGeometry*g){
        auto*skin=g->GetGeometryRuntimeData().skinInstance.get();
        auto*part=skin?skin->skinPartition.get():nullptr;
        out+=std::format("{}:{}:{};",reinterpret_cast<std::uintptr_t>(g),reinterpret_cast<std::uintptr_t>(skin),reinterpret_cast<std::uintptr_t>(part));
        return RE::BSVisit::BSVisitControl::kContinue;
    });return out;
}
bool Capable(const Live&target,const height_profiles::Capability&cap,hp::Controls values){
    if(cap.status!="parsed"||!hp::Valid(values))return false;
    // RaceMenu applies every shape record when its root is a BSGeometry. Do not
    // send a multi-shape TRI through that route. Nodes use exact shape names.
    if(target.root->AsGeometry()&&cap.shapes.size()!=1)return false;
    bool n=false,h=false;
    RE::BSVisit::TraverseScenegraphGeometries(target.root.get(),[&](RE::BSGeometry*g){
        auto*skin=g->GetGeometryRuntimeData().skinInstance.get();auto*part=skin?skin->skinPartition.get():nullptr;
        if(!part)return RE::BSVisit::BSVisitControl::kContinue;
        for(const auto&s:cap.shapes){
            const char* name=g->name.c_str();
            if(!target.root->AsGeometry()&&s.name!=(name?name:""))continue;
            if(s.maxIndex>=part->vertexCount)continue;
            n|=s.noHeel;h|=s.heel;
        }return RE::BSVisit::BSVisitControl::kContinue;
    });
    return (n||h)&&(values.noHeel==0||n)&&(values.heel==0||h);
}
struct Plan {hp::Controls values;std::string authority;};
std::optional<Plan> Manual(const Live&stock,const Live&shoe){
    auto it=config.find("signedHeightOverrides");
    std::optional<Plan> result;
    if(it!=config.end()&&it->is_array())for(const auto&row:*it){
        if(!row.is_object()||row.value("footwear",std::string{})!=shoe.visual.armorID||
           row.value("stocking",std::string{})!=stock.visual.armorID)continue;
        if(result){logger::warn("[height plan] duplicate manual pair; refusing {}",stock.visual.armorID);return Plan{{NAN,NAN},"ambiguous-manual"};}
        if(!row.contains("signedPosture")||!row.at("signedPosture").is_number())return Plan{{NAN,NAN},"invalid-manual"};
        auto controls=hp::FromSigned(row.at("signedPosture").get<double>());
        if(!controls)return Plan{{NAN,NAN},"invalid-manual"};
        result=Plan{*controls,"manual-pair"};
    }
    if(result)return result;
    auto heels=config.find("heels");if(heels!=config.end()&&heels->is_object()){
        auto entry=heels->find(shoe.visual.armorID);
        if(entry!=heels->end()){
            if(!entry->is_number())return Plan{{NAN,NAN},"invalid-legacy-manual"};
            const double q=entry->get<double>();
            if(!std::isfinite(q)||q<0||q>1)return Plan{{NAN,NAN},"invalid-legacy-manual"};
            return Plan{{q,0},"manual-legacy-NoHeel"};
        }
    }return {};
}
std::optional<Plan> Automatic(const Live&stock,const Live&shoe,const std::string&context,const height_profiles::Capability&cap){
    const auto logKey=stock.visual.armorID+"|"+shoe.visual.armorID;
    if(!Bool("automaticHeight",true)||context.empty())return {};
    auto p=height_profiles::Lookup(shoe.visual.armorID,shoe.visual.addonID,stock.visual.armorID,stock.visual.addonID,context);
    if(!p){
        DecisionLog(logKey,std::format("stocking='{}' shoe='{}' reason=no-current-validated-height-profile (not barefoot)",stock.visual.armorID,shoe.visual.armorID));
        return {};
    }
    if(p->at("bodyTriFingerprint")!=cap.fingerprint){
        DecisionLog(logKey,std::format("stocking='{}' shoe='{}' reason=stocking-TRI-changed",stock.visual.armorID,shoe.visual.armorID));
        return {};
    }
    double limit=.15;
    if(auto i=config.find("heightMaxNormalizedResidual");i!=config.end()&&i->is_number())limit=i->get<double>();
    if(!std::isfinite(limit)||limit<0||limit>1)return {};
    const double residual=p->at("normalizedResidual").get<double>();
    const bool saturated=p->at("saturated").get<bool>();
    if(residual>limit||(saturated&&!Bool("allowHeightEndpointApproximation",false))){
        DecisionLog(logKey,std::format("stocking='{}' shoe='{}' reason={} normalizedResidual={:.6f} limit={:.6f} saturated={} (not barefoot)",
            stock.visual.armorID,shoe.visual.armorID,residual>limit?"height-residual-too-large":"height-range-exceeded",residual,limit,saturated));
        return {};
    }
    return Plan{{p->at("NoHeel").get<double>(),p->at("Heel").get<double>()},
        saturated?"measured-endpoint-approximation":"measured-height"};
}
struct BarefootObservation {
    bool ready{};
    std::string decision,signature;
};
BarefootObservation ObserveBarefoot(RE::Actor* player,const api::VisualState001& state,
    const std::vector<Visual>& visuals,const std::vector<racemenu::BipedPartRecord>& parts,
    std::size_t liveShoes,const std::string& context)
{
    barefoot_policy::Evidence evidence;
    evidence.enabled=Bool("barefootFlatFeet",true);
    evidence.snapshotValid=(state.flags&api::kVisualState_LiveEvaluation)&&
        state.pieceCount<=4096&&(!state.pieceCount||state.pieces);
    evidence.liveFootwear=liveShoes;
    for(const auto& v:visuals)if(!v.stocking&&(v.slots&(1ULL<<7)))++evidence.declaredFootwear;
    // A structurally incomplete feet record is not evidence of an empty slot.
    if(evidence.snapshotValid)for(unsigned i=0;i<state.pieceCount;++i){
        const auto& p=state.pieces[i];
        if(p.structureSize<sizeof(api::VisualPiece001)) {evidence.snapshotValid=false;break;}
        if(!(p.flags&api::kVisualPiece_Hidden)&&(p.visualSlotMask&(1ULL<<7))&&
           (!p.replacementArmorFormID||!p.replacementArmorAddonFormID||!p.actorModelPath)){
            evidence.snapshotValid=false;break;
        }
    }
    BarefootObservation out;
    RE::FormID skinID=0,addonID=0;
    std::uintptr_t cloneID=0;
    std::string model,geometryStamp;
    auto* skin=player->GetSkin();
    auto* worn=player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet);
    evidence.realFootwearWorn=worn!=nullptr;
    if(skin)skinID=skin->GetFormID();
    const racemenu::BipedPartRecord* active=nullptr;
    unsigned activeCount=0;
    for(const auto& p:parts)if(!p.buffered&&p.slotNumber==37){++activeCount;active=&p;}
    if(activeCount==1&&active&&active->partClone&&skinID&&active->itemFormID==skinID){
        cloneID=reinterpret_cast<std::uintptr_t>(active->partClone.get());
        addonID=active->addonFormID;
        evidence.activeSkinPart=true;
        for(auto* addon:skin->armorAddons)if(addon&&addon->GetFormID()==addonID){
            evidence.skinAddonConfirmed=true;
            const auto* base=player->GetActorBase();
            if(base){
                const auto sex=base->GetSex()==RE::SEX::kFemale?1u:0u;
                const auto* path=addon->bipedModels[sex].GetModel();
                model=path?path:"";
            }
            break;
        }
        // Explicit known bare-foot model, not a substring such as "feet".
        const auto models=config.find("referenceFeetModels");
        if(models!=config.end()&&models->is_array())for(const auto& p:*models)
            if(p.is_string()&&foot_capture_policy::SameResource(model,p.get<std::string>()))evidence.expectedBareFeetModel=true;
        RE::BSVisit::TraverseScenegraphGeometries(active->partClone.get(),[&](RE::BSGeometry*g){
            if(g){auto* instance=g->GetGeometryRuntimeData().skinInstance.get();
                auto* partition=instance?instance->skinPartition.get():nullptr;
                if(partition&&partition->vertexCount>0&&partition->vertexCount<=65535)evidence.currentSkinnedGeometry=true;}
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        geometryStamp=Stamp(active->partClone.get());
    }
    out.decision=barefoot_policy::Decision(evidence);
    out.signature=Json::array({height_profiles::Session(),dirty.load(),skinID,addonID,cloneID,model,geometryStamp,context}).dump();
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    out.ready=barefootSettler.Ready(out.decision,out.signature,static_cast<std::uint64_t>(now));
    if(!out.ready&&out.decision=="confirmed-barefoot")out.decision="barefoot-waiting-for-stable-state";
    if(liveShoes==0)DecisionLog("footwear-state",std::format("footwear={} declared={} realWorn={} skin={:08X} addon={:08X} model='{}'",
        out.decision,evidence.declaredFootwear,evidence.realFootwearWorn,skinID,addonID,model));
    else lastDecision.erase("footwear-state");
    return out;
}
void RestoreUnclaimed(RE::Actor*player,const std::set<std::uintptr_t>&claimed){
    for(auto it=applications.begin();it!=applications.end();){
        if(claimed.contains(it->first)){++it;continue;}
        if(racemenu::IsLive(it->second.object.get())){
            const bool ok=racemenu::RestoreScopedMorphs(player,it->second.object.get());
            logger::info("[height reset] restored actor baseline={} (no valid footwear state/unsupported/disabled/visual removed)",ok);
        }
        it=applications.erase(it);
    }
}
void Visit(const api::VisualState001*state,void*){
    if(!state)return;
    auto*player=RE::PlayerCharacter::GetSingleton();if(!player||state->actorFormID!=player->GetFormID())return;
    if(state->interfaceRevision!=1||state->structureSize<sizeof(api::VisualState001)||
        state->pieceStructureSize!=sizeof(api::VisualPiece001))return;
    std::set<std::uintptr_t> claimed;
    if(!Bool("applyMorph",false)){barefootSettler.Reset();RestoreUnclaimed(player,claimed);return;}
    auto visuals=Visuals(*state);auto parts=racemenu::ScanPlayerBipedParts();
    auto live=MatchLive(visuals,parts);
    std::vector<const Live*> shoes,stockings;
    std::set<std::uintptr_t> seenShoes,seenStockings;
    for(const auto&v:live){auto id=reinterpret_cast<std::uintptr_t>(v.root.get());
        if(v.visual.stocking){if(seenStockings.insert(id).second)stockings.push_back(&v);}
        else if(seenShoes.insert(id).second)shoes.push_back(&v);
    }
    const auto context=racemenu::ActorMorphContext(player);
    if(stockings.empty()){barefootSettler.Reset();RestoreUnclaimed(player,claimed);return;}
    const auto barefoot=ObserveBarefoot(player,*state,visuals,parts,shoes.size(),context);
    if(shoes.size()==1||barefoot.ready)for(const auto*stock:stockings){
        const auto cap=height_profiles::RequestCapability(stock->tri);if(!cap)continue;
        std::optional<Plan> plan;
        std::string shoeID,shoeAddon,footwearSignature;
        if(shoes.size()==1){
            shoeID=shoes.front()->visual.armorID;shoeAddon=shoes.front()->visual.addonID;
            plan=Manual(*stock,*shoes.front());if(!plan)plan=Automatic(*stock,*shoes.front(),context,*cap);
        }else{
            shoeID="<barefoot>";footwearSignature=barefoot.signature;
            // Deliberate flat-foot policy, not a calibrated height-profile entry.
            plan=Plan{{1,0},"confirmed-barefoot-flat"};
        }
        if(!plan||!hp::Valid(plan->values))continue;
        if(!Capable(*stock,*cap,plan->values)){
            DecisionLog(stock->visual.armorID+"|"+shoeID,std::format("stocking='{}' shoe='{}' reason=required-morph-or-live-shape-unavailable",stock->visual.armorID,shoeID));
            continue;
        }
        const auto id=reinterpret_cast<std::uintptr_t>(stock->root.get());
        const auto signature=Json::array({shoeID,shoeAddon,stock->visual.armorID,
            stock->visual.addonID,plan->values.noHeel,plan->values.heel,context,cap->fingerprint,footwearSignature}).dump();
        const auto stamp=Stamp(stock->root.get());const auto revision=dirty.load();
        auto old=applications.find(id);
        if(old!=applications.end()&&old->second.desired==signature&&old->second.stamp==stamp&&old->second.dirty==revision){claimed.insert(id);continue;}
        const std::array<scoped_morph_transaction::Target,2> targets{{{"NoHeel",float(plan->values.noHeel)},{"Heel",float(plan->values.heel)}}};
        const bool ok=racemenu::ApplyScopedMorphs(player,stock->root.get(),targets,stock->visual.armorID);
        logger::info("[height apply] stocking='{}' shoe='{}' NoHeel={:.4f} Heel={:.4f} source={} submitted={}",
            stock->visual.armorID,shoeID,plan->values.noHeel,plan->values.heel,plan->authority,ok);
        if(ok){applications[id]={stock->root,signature,Stamp(stock->root.get()),revision};claimed.insert(id);lastDecision.erase(stock->visual.armorID+"|"+shoeID);}
    }
    RestoreUnclaimed(player,claimed);
}
bool Connect(){
    if(!svs)svs=api::GetSkyrimVanitySystemInterface001();
    if(!svs){if(!loggedUnavailable)logger::warn("[height runtime] waiting for SVS API 001");loggedUnavailable=true;return false;}
    loggedUnavailable=false;
    if(!listener)listener=svs->RegisterVisualStateChangedListener([](const api::VisualState001*,void*){Changed();foot_capture::RequestCapture();},nullptr);
    return listener!=0&&svs->IsReady();
}
void QueueSnapshot(){
    if(!running.load()||queued.exchange(true))return;
    auto* tasks=SKSE::GetTaskInterface();if(!tasks){queued.store(false);return;}
    const auto epoch=height_profiles::Session();
    tasks->AddTask([epoch]{
        queued.store(false);if(!running.load()||epoch!=height_profiles::Session())return;
        std::unique_lock lock(updateMutex,std::try_to_lock);if(!lock.owns_lock())return;
        try{if(!Connect()||!racemenu::Available())return;auto*player=RE::PlayerCharacter::GetSingleton();
            if(player)svs->VisitActorVisualState(player,Visit,nullptr);
        }catch(const std::exception&e){logger::warn("[height runtime] safe evaluation failure: {}",e.what());}
    });
}
// Slow condition/watchdog refresh, not per-frame polling. Also covers OBody
// updates that do not emit equip/SVS notifications. No game objects on this thread.
class Timer{
    std::mutex mutex;std::condition_variable cv;std::jthread worker;
public:
    Timer():worker([this](std::stop_token stop){std::unique_lock lock(mutex);while(!stop.stop_requested()){
        if(cv.wait_for(lock,std::chrono::seconds(1),[&]{return stop.stop_requested();}))break;
        lock.unlock();QueueSnapshot();lock.lock();
    }}){}
    ~Timer(){worker.request_stop();cv.notify_all();worker.join();}
};
void StartTimer(){static Timer timer;}
class Equip final:public RE::BSTEventSink<RE::TESEquipEvent>{
public:RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent*e,RE::BSTEventSource<RE::TESEquipEvent>*)override{
    auto*p=RE::PlayerCharacter::GetSingleton();if(e&&p&&e->actor.get()==p){Changed();foot_capture::RequestCapture();}
    return RE::BSEventNotifyControl::kContinue;
}} equip;
void Load(){
    std::unique_lock lock(updateMutex);
    try{std::ifstream f(kConfig);auto c=f?Json::parse(f):Json();if(!c.is_object())throw std::runtime_error("missing/invalid config");config=std::move(c);}
    catch(const std::exception&e){config=Json::object();config["applyMorph"]=false;logger::warn("[height runtime] disabled by invalid config: {}",e.what());}
    if(auto* player=RE::PlayerCharacter::GetSingleton())RestoreUnclaimed(player,{});
    applications.clear();barefootSettler.Reset();lastDecision.clear();racemenu::ResetAttachmentCache();
    racemenu::Initialize();racemenu::SetAttachmentChangedCallback(Changed);
    if(!registered){auto*h=RE::ScriptEventSourceHolder::GetSingleton();auto*s=h?h->GetEventSource<RE::TESEquipEvent>():nullptr;if(s){s->AddEventSink(&equip);registered=true;}}
    height_profiles::Initialize(QueueSnapshot);
    running.store(true);foot_capture::SetSessionReady(true);StartTimer();
    logger::info("[height runtime] active={} automatic={} branchRange=[0,1] endpointApproximation={} barefootFlatFeet={} (BodySlide full scan retired; clearance paused)",
        Bool("applyMorph",false),Bool("automaticHeight",true),Bool("allowHeightEndpointApproximation",false),Bool("barefootFlatFeet",true));
    lock.unlock();Changed();foot_capture::RequestCapture();
}
void Handle(SKSE::MessagingInterface::Message*message){
    if(!message)return;
    switch(message->type){
    case SKSE::MessagingInterface::kPostPostLoad:racemenu::Initialize();break;
    case SKSE::MessagingInterface::kPreLoadGame:
        running.store(false);foot_capture::SetSessionReady(false);height_profiles::BeginSession();break;
    case SKSE::MessagingInterface::kDataLoaded:
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        running.store(false);foot_capture::SetSessionReady(false);height_profiles::BeginSession();
        if(auto*t=SKSE::GetTaskInterface())t->AddTask(Load);break;
    default:break;
    }
}
}
}
extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface*skse){
    REL::Module::reset();SKSE::Init(skse);auto*m=SKSE::GetMessagingInterface();if(!m)return false;
    m->RegisterListener("SKSE",vanity_ube_heel_adapter::Handle);
    logger::info("{} {} loaded (transactional height controller)",VanityUBEHeelAdapterPlugin::NAME,VanityUBEHeelAdapterPlugin::VERSION.string());
    return true;
}
