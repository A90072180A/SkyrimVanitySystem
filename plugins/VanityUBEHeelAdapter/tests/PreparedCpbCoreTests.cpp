#include "../src/PreparedCpbCore.h"
#include <iostream>
#include <limits>
#include <stdexcept>
namespace p=vanity_ube_heel_adapter::prepared_cpb;
int checks=0;void check(bool v){if(!v)throw std::runtime_error("prepared policy check "+std::to_string(checks));++checks;}
int main(){
    check(p::ResourceMatches("Meshes/!UBE/Caenarvon/Cosplay/CPB_SB_1.nif",p::PreparedTri));
    check(p::ResourceMatches(p::Model0,p::PreparedTri));check(!p::ResourceMatches("other/cpb_sb_1.nif",p::PreparedTri));
    check(!p::ResourceMatches(p::Model1,p::OriginalTri));
    check(p::IsAgata(p::AgataArmor,p::AgataAddon,p::AgataModel));
    check(!p::IsAgata(p::AgataArmor,p::GlassAddon,p::AgataModel));
    check(p::IsGlass(p::GlassArmor,p::GlassAddon,p::GlassModel));
    check(!p::IsGlass("glass high-heeled shoes.esp|00000801",p::GlassAddon,p::GlassModel));
    check(!p::IsAgata(p::GlassArmor,p::GlassAddon,p::GlassModel));
    std::vector<p::Shape> shapes{{std::string(p::MainShape),23340,23339,true,true},{std::string(p::FootShape),23340,23339,true,true}};
    check(p::CompleteShapes(shapes));auto copy=shapes;copy[1].heel=false;check(!p::CompleteShapes(copy));
    copy=shapes;copy[1].noHeel=false;check(!p::CompleteShapes(copy));copy=shapes;copy[1].vertices=23339;check(!p::CompleteShapes(copy));
    copy=shapes;copy[0].maxIndex=23340;check(!p::CompleteShapes(copy));copy=shapes;copy[1].name=copy[0].name;check(!p::CompleteShapes(copy));
    copy=shapes;copy.push_back({"ForeignBody",23340,0,true,true});check(!p::CompleteShapes(copy));shapes.pop_back();check(!p::CompleteShapes(shapes));
    std::vector<std::string> blocked{"FeetSize","AnkleThickness"};std::vector<p::MorphValue> values;
    check(p::ContextSafe(0,1,values,blocked));check(!p::ContextSafe(.002,1,values,blocked));check(!p::ContextSafe(-.001,1,values,blocked));
    check(!p::ContextSafe(0,0,values,blocked));check(!p::ContextSafe(std::numeric_limits<double>::quiet_NaN(),1,values,blocked));
    values={{"ChestSize",.5}};check(p::ContextSafe(0,1,values,blocked));values.push_back({"FeetSize",.1});check(!p::ContextSafe(0,1,values,blocked));
    values.back().value=0;check(p::ContextSafe(0,1,values,blocked));values={{std::string(p::FitMorph),1}};check(!p::ContextSafe(0,1,values,blocked));
    values={{"ChestSize",std::numeric_limits<double>::infinity()}};check(!p::ContextSafe(0,1,values,blocked));
    check(p::AtHeight(0,1.1,1.1));check(!p::AtHeight(1,0,1.1));check(!p::AtHeight(0,.47,1.1));check(!p::AtHeight(.1,1.1,1.1));
    p::VisibilityLease lease;
    check(lease.Hide(false)==true);check(!lease.Hide(true));check(lease.Release(true)==false);check(!lease.owned);
    check(!lease.Hide(true));check(!lease.Release(true));
    check(lease.Hide(false)==true);check(!lease.Hide(false));check(lease.yielded);check(!lease.Hide(false));check(!lease.Release(false));
    check(lease.Hide(false)==true);check(!lease.Release(false));
    check(!lease.Release(true));
    std::cout<<checks<<" prepared CPB policy checks passed\n";
}
