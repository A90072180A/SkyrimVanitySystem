#include "../src/StockingOcclusionPolicy.h"
#include <iostream>
using vanity_ube_heel_adapter::stocking_occlusion::ShouldHide;
int main(){int c=0;auto q=[&](bool v){++c;if(!v)throw c;};
q(ShouldHide(true,1,"opaque-closed"));q(!ShouldHide(false,1,"opaque-closed"));
q(!ShouldHide(true,0,"opaque-closed"));q(!ShouldHide(true,2,"opaque-closed"));
q(!ShouldHide(true,1,"preserve"));q(!ShouldHide(true,1,""));
std::cout<<c<<" stocking occlusion policy checks passed\n";}
