#include "ConfigState.h"
#include "ConfigurationCore.h"
#include "RuntimeFiles.h"
#include "Plugin.h"
#include "RaceMenuBridge.h"
#include "FootGeometryCapture.h"
#include "HeightProfiles.h"
#include "BulkHeightLibrary.h"
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
std::uint64_t configRevision=0;
std::string measurementContext;
double HeelMax(){return config.value("heelMax",2.0);}
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
void AcceptConfiguration(Json next);
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
    std::uint64_t slots{};bool stocking{},ignored{},markedFootwear{};
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
std::string Mark(const std::string& id){
    auto kinds=config.find("manualItemKinds");if(kinds!=config.end()&&kinds->is_object()){
        auto found=kinds->find(id);if(found!=kinds->end()&&found->is_string())return found->get<std::string>();
    }return {};
}
bool IsStocking(const Visual&v){
    const auto mark=Mark(v.armorID);if(!mark.empty())return mark=="stocking";
    if(ExplicitStocking(v.armorID))return true;
    if(Bool("useOfflineHeightLibrary",true)&&bulk_height::IsStocking({v.armorID,v.addonID,v.model}))return true;
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
        v.stocking=IsStocking(v);v.ignored=Mark(v.armorID)=="ignore";v.markedFootwear=Mark(v.armorID)=="footwear";
        if((v.stocking||v.markedFootwear||v.ignored||(v.slots&(1ULL<<7)))&&seen.emplace(v.armor,v.addon,v.model).second)out.push_back(std::move(v));
    }
    return out;
}
std::vector<Live> MatchLive(const std::vector<Visual>& visuals,const std::vector<racemenu::BipedPartRecord>&parts){
    std::vector<Live> out;
    for(const auto&v:visuals){if(v.ignored)continue;std::map<std::uintptr_t,Live> matches;
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
    if(cap.status!="parsed"||!hp::Valid(values,HeelMax()))return false;
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
struct Plan {hp::Controls values;std::string authority;Json metadata=Json::object();};
std::optional<Plan> UserPair(const Live& stock,const std::string& shoe,const std::string& shoeAddon,const std::string& context,const height_profiles::Capability& cap){
    for(const auto& row:config.value("manualPairs",Json::array())){
        if(row.at("stocking")!=stock.visual.armorID || row.at("footwear")!=shoe)continue;
        if(row.value("mode",std::string{"manual"})=="ignore")return Plan{{NAN,NAN},"user-pair-ignored"};
        if(row.contains("approval")){
            const auto& a=row.at("approval");
            auto measured=height_profiles::Lookup(shoe,shoeAddon,stock.visual.armorID,stock.visual.addonID,context);
            bool valid=measured.has_value()&&a.at("bodyTriFingerprint")==cap.fingerprint&&a.at("stockingAddon")==stock.visual.addonID&&a.at("footwearAddon")==shoeAddon;
            if(valid)for(const auto* key:{"context","targetPositionFingerprint","donorSourceFingerprint","bodyTriFingerprint","referenceConfiguration"})if(a.at(key)!=measured->at(key))valid=false;
            if(!valid)return Plan{{NAN,NAN},"user-approval-stale-or-awaiting-current-sources"};
        }
        return Plan{{row.at("NoHeel").get<double>(),row.at("Heel").get<double>()},row.contains("approval")?"user-approved-measurement":"user-manual-pair"};
    }return {};
}
std::optional<Plan> Manual(const Live&stock,const Live&shoe){
    auto it=config.find("signedHeightOverrides");
    std::optional<Plan> result;
    if(it!=config.end()&&it->is_array())for(const auto&row:*it){
        if(!row.is_object()||row.value("footwear",std::string{})!=shoe.visual.armorID||
           row.value("stocking",std::string{})!=stock.visual.armorID)continue;
        if(result){logger::warn("[height plan] duplicate manual pair; refusing {}",stock.visual.armorID);return Plan{{NAN,NAN},"ambiguous-manual"};}
        if(!row.contains("signedPosture")||!row.at("signedPosture").is_number())return Plan{{NAN,NAN},"invalid-manual"};
        auto controls=hp::FromSigned(row.at("signedPosture").get<double>(),HeelMax());
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
std::optional<Plan> OfflineLibrary(const Live& stock,const Live& shoe,const std::string& context){
    if(!Bool("useOfflineHeightLibrary",true)||context.empty())return {};
    try {
        const auto actor=Json::parse(context);
        if(!actor.is_array()||actor.size()!=4||actor.at(1)!=1)return {};
        bulk_height::Request request{{shoe.visual.armorID,shoe.visual.addonID,shoe.visual.model},
            {stock.visual.armorID,stock.visual.addonID,stock.visual.model},context,
            actor.at(2).get<double>(),HeelMax(),Bool("applyHeightResidualWarnings",true)};
        auto reply=bulk_height::Lookup(std::move(request));
        // Missing libraries/entries allow the established automatic path. A
        // selected but invalid/pending library entry is not silently replaced.
        if(reply.state=="unavailable"||reply.state=="not-found")return {};
        Json info={{"state",reply.state},{"generation",reply.generation},{"flags",reply.flags},
            {"originalResidual",reply.originalResidual},{"detail",reply.detail},
            {"offlineBaselineOnly",true},{"sharedValueApproximation",(reply.flags&2)!=0},{"residualWarning",(reply.flags&1)!=0},
            {"residualAtAppliedValue",(reply.flags&6)?Json(nullptr):Json(reply.originalResidual)}};
        if(!reply.Ready())return Plan{{NAN,NAN},"offline-library-"+reply.state,std::move(info)};
        return Plan{{reply.values.noHeel,reply.values.heel},
            (reply.flags&1)?"offline-library-residual-warning":"offline-library",std::move(info)};
    }catch(const std::exception& e){return Plan{{NAN,NAN},"offline-library-context-invalid",{{"error",e.what()}}};}
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
    if(!hp::Valid({p->at("NoHeel").get<double>(),p->at("Heel").get<double>()},HeelMax())){
        DecisionLog(logKey,"cached controls exceed current asymmetric limits");return {};
    }
    const double residual=p->at("normalizedResidual").get<double>();
    const bool saturated=p->at("saturated").get<bool>();
    if((residual>limit&&!Bool("applyHeightResidualWarnings",true))||(saturated&&!Bool("allowHeightEndpointApproximation",false))){
        DecisionLog(logKey,std::format("stocking='{}' shoe='{}' reason={} normalizedResidual={:.6f} limit={:.6f} saturated={} (not barefoot)",
            stock.visual.armorID,shoe.visual.armorID,(saturated&&!Bool("allowHeightEndpointApproximation",false))?"height-range-exceeded":"height-residual-too-large",residual,limit,saturated));
        return {};
    }
    return Plan{{p->at("NoHeel").get<double>(),p->at("Heel").get<double>()},
        saturated?"measured-endpoint-approximation":residual>limit?"measured-height-residual-warning":"measured-height",
        {{"normalizedResidual",residual},{"residualLimit",limit},{"residualWarning",residual>limit}}};
}
struct BarefootObservation {
    bool ready{};
    std::string decision,signature;
    Json evidence;
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
    for(const auto& v:visuals)if(!v.stocking&&((v.slots&(1ULL<<7))||v.markedFootwear))++evidence.declaredFootwear;
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
    RE::FormID skinID=0,addonID=0,wornID=0;
    std::uintptr_t cloneID=0;
    std::string model,geometryStamp;
    auto* skin=player->GetSkin();
    auto* worn=player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet);
    evidence.realFootwearWorn=worn!=nullptr;
    if(worn)wornID=worn->GetFormID();
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
    out.evidence={{"snapshotValid",evidence.snapshotValid},{"declaredFootwear",evidence.declaredFootwear},
        {"liveFootwear",evidence.liveFootwear},{"wornQueryID",Stable(wornID)},{"wornQueryIsSkin",wornID!=0&&wornID==skinID},
        {"skinID",Stable(skinID)},{"activeSkinPart",evidence.activeSkinPart},{"skinAddonConfirmed",evidence.skinAddonConfirmed},
        {"expectedBareFeetModel",evidence.expectedBareFeetModel},{"currentSkinnedGeometry",evidence.currentSkinnedGeometry},
        {"addonID",Stable(addonID)},{"model",model},{"stable",out.ready}};
    if(liveShoes==0)DecisionLog("footwear-state",std::format("footwear={} declared={} wornQuery={:08X} queryIsSkin={} skin={:08X} addon={:08X} model='{}' activeSkin={} skinAddon={} expectedModel={} skinned={}",
        out.decision,evidence.declaredFootwear,wornID,wornID!=0&&wornID==skinID,skinID,addonID,model,
        evidence.activeSkinPart,evidence.skinAddonConfirmed,evidence.expectedBareFeetModel,evidence.currentSkinnedGeometry));
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
    if(state->interfaceRevision!=1||state->structureSize<sizeof(api::VisualState001)||state->pieceStructureSize!=sizeof(api::VisualPiece001))return;
    auto visuals=Visuals(*state);auto parts=racemenu::ScanPlayerBipedParts();auto live=MatchLive(visuals,parts);
    Json report={{"schema",1},{"generatorVersion","0.18.0"},{"session",height_profiles::Session()},
        {"configurationRevision",configRevision},{"heelMax",HeelMax()},{"NoHeelMaximum",1.0},
        {"active",Bool("applyMorph",false)},{"visuals",Json::array()},{"decisions",Json::array()}};
    const auto libraryIO=bulk_height::Counters();
    report["offlineLibraryIO"]={{"indexReads",libraryIO.indexReads},{"shoeReads",libraryIO.shardReads},{"assetReads",libraryIO.assetReads}};
    for(const auto&v:visuals){
        Json row={{"armor",v.armorID},{"addon",v.addonID},{"model",v.model},{"visualSlots",v.slots},
            {"kind",v.ignored?"ignore":v.stocking?"stocking":"footwear"},{"manualMark",Mark(v.armorID)},{"liveMatched",false}};
        for(const auto& l:live)if(l.visual.armor==v.armor&&l.visual.addon==v.addon&&l.visual.model==v.model){row["liveMatched"]=true;row["bodyTri"]=l.tri;}
        report["visuals"].push_back(std::move(row));
    }
    std::set<std::uintptr_t> claimed;
    auto finish=[&]{RestoreUnclaimed(player,claimed);if(Bool("writeRuntimeState",true))runtime_files::Status(std::move(report));};
    if(!Bool("applyMorph",false)){barefootSettler.Reset();report["state"]="controller-disabled";finish();return;}
    std::vector<const Live*> shoes,stockings;std::set<std::uintptr_t> seenShoes,seenStockings;
    for(const auto&v:live){auto id=reinterpret_cast<std::uintptr_t>(v.root.get());
        if(v.visual.stocking){if(seenStockings.insert(id).second)stockings.push_back(&v);}
        else if(seenShoes.insert(id).second)shoes.push_back(&v);}
    const auto context=racemenu::ActorMorphContext(player);
    if(context!=measurementContext){measurementContext=context;foot_capture::RequestCapture();}
    if(stockings.empty()){barefootSettler.Reset();report["state"]="no-live-stocking";finish();return;}
    const auto barefoot=ObserveBarefoot(player,*state,visuals,parts,shoes.size(),context);
    report["barefootEvidence"]=barefoot.evidence;
    report["state"]=shoes.size()==1?"one-live-footwear":barefoot.decision;
    if(shoes.size()==1||barefoot.ready)for(const auto*stock:stockings){
        const auto cap=height_profiles::RequestCapability(stock->tri);
        const std::string shoeID=shoes.size()==1?shoes.front()->visual.armorID:"<barefoot>";
        const std::string shoeAddon=shoes.size()==1?shoes.front()->visual.addonID:"";
        Json item={{"stocking",stock->visual.armorID},{"stockingAddon",stock->visual.addonID},{"footwear",shoeID},
            {"footwearAddon",shoeAddon},{"bodyTri",stock->tri},{"submitted",false},{"NoHeel",nullptr},{"Heel",nullptr}};
        if(!cap){item["reason"]="capability-pending";report["decisions"].push_back(item);continue;}
        item["capabilityStatus"]=cap->status;item["bodyTriFingerprint"]=cap->fingerprint;
        auto plan=UserPair(*stock,shoeID,shoeAddon,context,*cap);
        if(!plan&&shoes.size()==1){plan=Manual(*stock,*shoes.front());if(!plan)plan=OfflineLibrary(*stock,*shoes.front(),context);if(!plan)plan=Automatic(*stock,*shoes.front(),context,*cap);}
        if(!plan&&barefoot.ready)plan=Plan{{1,0},"confirmed-barefoot-flat"};
        if(plan&&!plan->metadata.empty())item["heightEvidence"]=plan->metadata;
        if(!plan||!hp::Valid(plan->values,HeelMax())){
            auto key=stock->visual.armorID+"|"+shoeID;
            item["reason"]=plan?plan->authority:lastDecision.contains(key)?lastDecision.at(key):"no-accepted-height-plan";
            report["decisions"].push_back(item);continue;
        }
        item["NoHeel"]=plan->values.noHeel;item["Heel"]=plan->values.heel;item["authority"]=plan->authority;
        if(!Capable(*stock,*cap,plan->values)){
            item["reason"]="required-morph-or-live-shape-unavailable";report["decisions"].push_back(item);continue;
        }
        const auto id=reinterpret_cast<std::uintptr_t>(stock->root.get());
        const auto signature=Json::array({shoeID,shoeAddon,stock->visual.armorID,stock->visual.addonID,
            plan->values.noHeel,plan->values.heel,context,cap->fingerprint,barefoot.ready?barefoot.signature:std::string{},plan->authority,configRevision}).dump();
        const auto stamp=Stamp(stock->root.get());const auto revision=dirty.load();auto old=applications.find(id);
        if(old!=applications.end()&&old->second.desired==signature&&old->second.stamp==stamp&&old->second.dirty==revision){
            claimed.insert(id);item["submitted"]=true;item["reason"]="managed-current";report["decisions"].push_back(item);continue;}
        const std::array<scoped_morph_transaction::Target,2> targets{{{"NoHeel",float(plan->values.noHeel)},{"Heel",float(plan->values.heel)}}};
        const bool ok=racemenu::ApplyScopedMorphs(player,stock->root.get(),targets,stock->visual.armorID);
        logger::info("[height apply] stocking='{}' shoe='{}' NoHeel={:.4f} Heel={:.4f} source={} submitted={}",
            stock->visual.armorID,shoeID,plan->values.noHeel,plan->values.heel,plan->authority,ok);
        item["submitted"]=ok;item["reason"]=ok?"managed-current":"morph-submission-failed";report["decisions"].push_back(item);
        if(ok){applications[id]={stock->root,signature,Stamp(stock->root.get()),revision};claimed.insert(id);lastDecision.erase(stock->visual.armorID+"|"+shoeID);}
    }
    finish();
}
bool Connect(){
    if(!svs)svs=api::GetSkyrimVanitySystemInterface001();
    if(!svs){if(!loggedUnavailable)logger::warn("[height runtime] waiting for SVS API 001");loggedUnavailable=true;return false;}
    loggedUnavailable=false;
    if(!listener)listener=svs->RegisterVisualStateChangedListener([](const api::VisualState001*,void*){Changed();foot_capture::RequestCapture();},nullptr);
    return listener!=0&&svs->IsReady();
}
// GLOB is an explicit, self-clearing vanilla console mailbox. No command-table
// patching and no Papyrus/ConsoleUtil dependency. It is read only on game tasks.
RE::TESGlobal* ReloadMailbox(){
    auto* data=RE::TESDataHandler::GetSingleton();
    return data?data->LookupForm<RE::TESGlobal>(0x800,"VanityUBEHeelAdapter.Commands.esp"):nullptr;
}
void ConsoleMessage(const std::string& message){
    logger::info("[manual reload] {}",message);
    if(auto* console=RE::ConsoleLog::GetSingleton())console->Print("%s",message.c_str());
}
unsigned forcedPasses=0;
std::chrono::steady_clock::time_point nextForcedPass{};
void PollReloadMailbox(){
    auto* command=ReloadMailbox();if(!command||command->value==0.0F)return;
    const auto value=command->value;command->value=0.0F;
    if(value==1.0F){
        const auto id=runtime_files::RequestReload();
        ConsoleMessage(std::format("VHA reload {} queued. Close the console to resume game tasks.",id));
    }else if(value==2.0F){
        ConsoleMessage(std::format("VHA 0.18.0 revision={} enabled={} HeelMax={}. See configuration-status.json and runtime-state.json.",configRevision,Bool("applyMorph",false),HeelMax()));
    }else ConsoleMessage("VHA: set VHA_Reload to 1 = reread + reapply; to 2 = status. Other values ignored.");
}
void QueueSnapshot(){
    if(!running.load()||queued.exchange(true))return;
    auto* tasks=SKSE::GetTaskInterface();if(!tasks){queued.store(false);return;}
    const auto epoch=height_profiles::Session();
    tasks->AddTask([epoch]{
        queued.store(false);if(!running.load()||epoch!=height_profiles::Session())return;
        std::unique_lock lock(updateMutex,std::try_to_lock);if(!lock.owns_lock())return;
        try{
            PollReloadMailbox();
            if(auto forced=runtime_files::TakeForcedReload()){
                // Deliberately unconditional: equal configuration still clears the
                // managed-state dedupe and requests fresh live targets.
                AcceptConfiguration(std::move(forced->effective));
                runtime_files::Acknowledge(config,configRevision);
                runtime_files::AcknowledgeForcedReload(forced->requestId,configRevision);
                forcedPasses=3;nextForcedPass=std::chrono::steady_clock::now();
                ConsoleMessage(std::format("VHA reload {} accepted as revision {}; local reapply scheduled.",forced->requestId,configRevision));
            }else if(auto next=runtime_files::TakePending()){
                if(*next!=config)AcceptConfiguration(std::move(*next));
                runtime_files::Acknowledge(config,configRevision);
            }
            if(forcedPasses&&std::chrono::steady_clock::now()>=nextForcedPass){
                dirty.fetch_add(1);--forcedPasses;
                nextForcedPass=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
            }
            if(!Connect()||!racemenu::Available())return;auto*player=RE::PlayerCharacter::GetSingleton();
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
void AcceptConfiguration(Json next){
    // Caller owns updateMutex in an SKSE task. Workers never touch game objects.
    if(auto* player=RE::PlayerCharacter::GetSingleton())RestoreUnclaimed(player,{});
    foot_capture::SetSessionReady(false);
    config=std::move(next);config_state::Set(config);++configRevision;
    height_profiles::BeginSession();bulk_height::Reset();applications.clear();measurementContext.clear();barefootSettler.Reset();lastDecision.clear();
    foot_capture::SetSessionReady(true);dirty.fetch_add(1);foot_capture::RequestCapture();
    logger::info("[config reload] accepted revision={} Heel=[0,{}] NoHeel=[0,1] cached context invalidated",configRevision,HeelMax());
}
void Load(){
    std::unique_lock lock(updateMutex);
    forcedPasses=0;
    if(auto* command=ReloadMailbox())command->value=0.0F;
    if(auto next=runtime_files::ReadInitial())config=std::move(*next);
    else if(config.empty()){config={{"applyMorph",false},{"automaticHeight",false},{"heelMax",2.0}};}
    config_state::Set(config);++configRevision;
    runtime_files::Acknowledge(config,configRevision);
    height_profiles::BeginSession();bulk_height::Reset();
    if(auto* player=RE::PlayerCharacter::GetSingleton())RestoreUnclaimed(player,{});
    applications.clear();measurementContext.clear();barefootSettler.Reset();lastDecision.clear();racemenu::ResetAttachmentCache();
    racemenu::Initialize();racemenu::SetAttachmentChangedCallback(Changed);
    if(!registered){auto*h=RE::ScriptEventSourceHolder::GetSingleton();auto*s=h?h->GetEventSource<RE::TESEquipEvent>():nullptr;if(s){s->AddEventSink(&equip);registered=true;}}
    height_profiles::Initialize(QueueSnapshot);bulk_height::Initialize(QueueSnapshot);runtime_files::Initialize(QueueSnapshot);
    running.store(true);foot_capture::SetSessionReady(true);StartTimer();
    logger::info("[height runtime] active={} automatic={} HeelRange=[0,{}] NoHeelRange=[0,1] endpointApproximation={} barefootFlatFeet={} (BodySlide full scan retired; clearance paused)",
        Bool("applyMorph",false),Bool("automaticHeight",true),HeelMax(),Bool("allowHeightEndpointApproximation",false),Bool("barefootFlatFeet",true));
    lock.unlock();Changed();foot_capture::RequestCapture();
}
void Handle(SKSE::MessagingInterface::Message*message){
    if(!message)return;
    switch(message->type){
    case SKSE::MessagingInterface::kPostPostLoad:racemenu::Initialize();break;
    case SKSE::MessagingInterface::kPreLoadGame:
        running.store(false);foot_capture::SetSessionReady(false);height_profiles::BeginSession();bulk_height::Reset();break;
    case SKSE::MessagingInterface::kDataLoaded:
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        running.store(false);foot_capture::SetSessionReady(false);height_profiles::BeginSession();bulk_height::Reset();
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
