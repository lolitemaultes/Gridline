// Gridline — definitions of the VST3 interface ids the host uses (the SDK
// declares them in its headers; one translation unit must define them).
// FUnknown, IPluginBase, IPluginFactory*, IBStream come from the SDK's
// coreiids.cpp, built alongside.
#include "Vst3Common.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstunits.h"

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
DEF_CLASS_IID(IPlugViewContentScaleSupport)
namespace Linux {
DEF_CLASS_IID(IEventHandler)
DEF_CLASS_IID(ITimerHandler)
DEF_CLASS_IID(IRunLoop)
}  // namespace Linux
namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IProcessContextRequirements)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IEditController2)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IComponentHandler2)
DEF_CLASS_IID(IConnectionPoint)
DEF_CLASS_IID(IMessage)
DEF_CLASS_IID(IAttributeList)
DEF_CLASS_IID(IHostApplication)
DEF_CLASS_IID(IPlugInterfaceSupport)
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IEventList)
DEF_CLASS_IID(INoteExpressionController)
DEF_CLASS_IID(IUnitInfo)
DEF_CLASS_IID(IMidiMapping)
}  // namespace Vst
}  // namespace Steinberg
