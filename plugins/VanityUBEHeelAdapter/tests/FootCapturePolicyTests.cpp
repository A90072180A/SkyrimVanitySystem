#include "../src/FootCapturePolicy.h"
#include <cstdlib>
#include <iostream>
using namespace vanity_ube_heel_adapter::foot_capture_policy;
int main()
{
    int checks = 0;
    auto check = [&](bool value) { ++checks; if (!value) { std::cerr << "failed check " << checks << '\n'; std::exit(1); } };
    check(ResourceKey("!UBE\\Feet\\femalefeet_tangent.tri") == "!ube/feet/femalefeet_tangent.tri");
    check(SameResource("Data/Meshes/!UBE/Feet/feet.tri", "!ube\\feet\\feet.tri"));
    check(SameResource("./meshes/!UBE//Feet/feet.tri", "!UBE/Feet/feet.tri"));
    check(!SameResource("!ube/feet/feet.tri", "other/feet.tri"));
    check(!SameResource("", ""));
    check(ResourceKey("../feet.tri").empty());
    check(ResourceKey("!ube/../feet.tri").empty());
    check(ResourceKey("C:\\feet.tri").empty());
    check(ResourceKey("/feet.tri").empty());
    check(ResourceKey("\\\\server\\feet.tri").empty());
    check(ResourceKey("data/meshes/").empty());
    const std::vector<std::string> expected{"!UBE/Feet/feet.tri"};
    std::vector<Evidence> live{{42,true,true,{"!ube\\feet\\feet.tri"}}};
    check(UniqueResourceMatch(live,expected) == 42);
    live.push_back(live.front()); check(UniqueResourceMatch(live,expected) == 42);
    live.push_back({43,true,true,{"!UBE/Feet/feet.tri"}}); check(!UniqueResourceMatch(live,expected));
    live.back().supportedGeometry = false; check(UniqueResourceMatch(live,expected) == 42);
    live = {{42,false,true,expected}}; check(!UniqueResourceMatch(live,expected));
    live = {{42,true,true,{"different/feet.tri"}}}; check(!UniqueResourceMatch(live,expected));
    live = {{0,true,true,expected}}; check(!UniqueResourceMatch(live,expected));
    check(!CurrentTicket(1,2)); check(CurrentTicket(2,2)); check(!CurrentTicket(0,0));
    check(RetryMilliseconds[0] > 0 && RetryMilliseconds[0] < RetryMilliseconds[1] && RetryMilliseconds[1] < RetryMilliseconds[2]);
    std::cout << checks << " selection checks passed\n";
}
