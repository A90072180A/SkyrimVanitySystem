#pragma once
#include "FootSnapshotCore.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <string>
namespace vanity_ube_heel_adapter::native_foot_fit_core {
using Point=foot_snapshot_core::Point;
struct Result{
 std::string status{"not-run"};double first{},second{},rms{},maxError{},basisRms{},relativeResidual{},normalizedDeterminant{};
};
inline Result Fit(std::span<const Point> observed,std::span<const Point> reference,
                  std::span<const Point> correction,std::span<const Point> first,std::span<const Point> second){
 Result out;const auto n=observed.size();
 if(!n||n>65535||reference.size()!=n||correction.size()!=n||first.size()!=n||second.size()!=n){out.status="invalid-array-size";return out;}
 double aa=0,bb=0,ab=0,ay=0,by=0,yy=0;
 for(std::size_t i=0;i<n;++i)for(unsigned c=0;c<3;++c){double a=first[i][c],b=second[i][c],y=double(observed[i][c])-reference[i][c]-correction[i][c];
  if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(y)){out.status="nonfinite-input";return out;}aa+=a*a;bb+=b*b;ab+=a*b;ay+=a*y;by+=b*y;yy+=y*y;}
 const double det=aa*bb-ab*ab;
 if(aa<=1e-20||bb<=1e-20||det<=aa*bb*1e-10){out.status="degenerate-basis";return out;}
 out.normalizedDeterminant=det/(aa*bb);out.first=(ay*bb-by*ab)/det;out.second=(by*aa-ay*ab)/det;
 double error=0;
 for(std::size_t i=0;i<n;++i){double d2=0;for(unsigned c=0;c<3;++c){double d=double(observed[i][c])-reference[i][c]-correction[i][c]-out.first*first[i][c]-out.second*second[i][c];d2+=d*d;}error+=d2;out.maxError=(std::max)(out.maxError,std::sqrt(d2));}
 out.rms=std::sqrt(error/double(n));out.basisRms=std::sqrt(aa/double(n));out.relativeResidual=yy>1e-20?std::sqrt(error/yy):0;
 out.status="diagnostic-fit";return out;
}
}
