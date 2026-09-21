#pragma once
class IBodyMorphInterface;
namespace vanity_ube_heel_adapter::foot_capture {
// Interface and runtime objects are accessed only in SKSE tasks. Writer owns copies.
void SetMorphInterface(IBodyMorphInterface* a_interface);
void RequestCapture();
}
