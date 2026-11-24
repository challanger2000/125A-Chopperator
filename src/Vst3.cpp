#include "HostEngine.hpp"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/base/ustring.h"
#include "base/source/fstreamer.h"
#include <string>
#include <type_traits>

namespace chopper {
using namespace Steinberg;
using namespace Steinberg::Vst;
static const FUID processorUID(0x9E3CD521,0xCD4A4D86,0xB855E192,0x3BEA0D41);
static const FUID controllerUID(0x3FA4C7C1,0x951A4C05,0xA31B85B0,0x5FCA2602);
constexpr uint32 magic=0x31504843u;
bool decode(IBStream* stream,Parameters& result){
    if(!stream)return false;
    IBStreamer s(stream,kLittleEndian);
    uint32 tag=0,version=0;
    if(!s.readInt32u(tag)||!s.readInt32u(version)||tag!=magic||version!=1)return false;
    Parameters staged;
    for(std::size_t i=0;i<parameterCount;++i){
        double x=0;
        if(!s.readDouble(x)||!std::isfinite(x)||x<0||x>1||!staged.set(static_cast<Param>(100+i),x))return false;
    }
    result=staged;return true;
}
bool encode(IBStream* stream,const Parameters& values){
    if(!stream)return false;
    IBStreamer s(stream,kLittleEndian);
    if(!s.writeInt32u(magic)||!s.writeInt32u(1))return false;
    for(std::size_t i=0;i<parameterCount;++i)if(!s.writeDouble(values.get(static_cast<Param>(100+i))))return false;
    return true;
}

// Each lane streams ordered points with fixed storage. Continuous automation
// follows SDK piecewise-linear segments; discrete commands change at offsets.
struct Lane{
    IParamValueQueue* queue{nullptr};
    Param id{Param::start};
    int32 count{0},index{0},last{-1},next{-1};
    double previous{0},target{0};
    bool discrete{false},bad{false};
    void fetch(int32 frames) noexcept {
        if(index>=count){next=-1;return;}
        int32 offset=0;ParamValue value=0;
        if(queue->getPoint(index++,offset,value)!=kResultTrue||offset<=last||
            offset<0||offset>=std::max(1,frames)||!std::isfinite(value)||value<0||value>1){
            bad=true;next=-1;return;
        }
        next=offset;target=value;
    }
    bool init(IParamValueQueue* q,int32 frames,const Parameters& p) noexcept {
        *this=Lane{};
        queue=q;
        const auto raw=q->getParameterId();
        if(raw<100||raw>=100+parameterCount)return false;
        id=static_cast<Param>(raw);discrete=steps[raw-100]!=0;
        previous=p.get(id);count=q->getPointCount();
        if(count<0||count>std::min(65536,std::max(1,frames))){bad=true;return false;}
        fetch(frames);return !bad;
    }
    void at(int32 frame,int32 frames,HostEngine& engine) noexcept {
        if(next<0)return;
        if(frame==next){
            engine.set(id,target);previous=target;last=next;fetch(frames);
        }else if(!discrete){
            const double fraction=double(frame-last)/double(next-last);
            engine.set(id,previous+(target-previous)*fraction);
        }
    }
};
class Processor final:public AudioEffect{
public:
    Processor(){setControllerClass(controllerUID);processContextRequirements.needTempo().needTimeSignature();}
    static FUnknown* createInstance(void*){return static_cast<IAudioProcessor*>(new Processor);}
    tresult PLUGIN_API initialize(FUnknown* context) override{
        const auto r=AudioEffect::initialize(context);if(r!=kResultOk)return r;
        addAudioInput(STR16("Input"),SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"),SpeakerArr::kStereo);return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in,int32 ni,SpeakerArrangement* out,int32 no) override{
        if(ni!=1||no!=1||!in||!out||in[0]!=out[0]||
            (in[0]!=SpeakerArr::kMono&&in[0]!=SpeakerArr::kStereo))return kResultFalse;
        return AudioEffect::setBusArrangements(in,ni,out,no);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override{
        return size==kSample32||size==kSample64?kResultTrue:kResultFalse;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup& s) override{
        if(!engine_.prepare(s.sampleRate))return kInvalidArgument;
        rate_=s.sampleRate;return AudioEffect::setupProcessing(s);
    }
    tresult PLUGIN_API setActive(TBool state) override{
        if(state&&!engine_.prepare(rate_))return kResultFalse;
        return AudioEffect::setActive(state);
    }
    tresult PLUGIN_API setProcessing(TBool) override{return kResultOk;}
    tresult PLUGIN_API setState(IBStream* stream) override{
        Parameters p;if(!decode(stream,p))return kResultFalse;
        engine_.restore(p);return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* stream) override{
        return encode(stream,engine_.save())?kResultOk:kResultFalse;
    }
    tresult PLUGIN_API process(ProcessData& data) override{
        if(data.numSamples<0)return kInvalidArgument;
        engine_.adopt();
        if(auto* c=data.processContext){
            const double bpm=(c->state&ProcessContext::kTempoValid)?c->tempo:bpm_;
            const int32 n=(c->state&ProcessContext::kTimeSigValid)?c->timeSigNumerator:n_;
            const int32 d=(c->state&ProcessContext::kTimeSigValid)?c->timeSigDenominator:d_;
            if(engine_.clock(bpm,n,d)){bpm_=bpm;n_=n;d_=d;}
        }
        std::array<Lane,parameterCount> lanes{};
        std::array<bool,parameterCount> seen{};
        std::size_t used=0;bool malformed=false;
        if(auto* changes=data.inputParameterChanges){
            const int32 count=changes->getParameterCount();
            if(count<0||count>64)return kResultFalse;
            for(int32 i=0;i<count;++i){
                auto* q=changes->getParameterData(i);if(!q)continue;
                const auto id=q->getParameterId();
                if(id<100||id>=100+parameterCount)continue;
                if(seen[id-100]){malformed=true;continue;}
                seen[id-100]=true;
                if(lanes[used].init(q,data.numSamples,engine_.parameters()))++used;
                else malformed=true;
            }
        }
        if(data.numSamples==0){
            for(std::size_t i=0;i<used;++i)lanes[i].at(0,0,engine_);
        }else if(data.symbolicSampleSize==kSample32){
            render<float>(data,lanes,used);
        }else if(data.symbolicSampleSize==kSample64){
            render<double>(data,lanes,used);
        }else return kResultFalse;
        engine_.publish();
        for(std::size_t i=0;i<used;++i)malformed|=lanes[i].bad;
        return malformed?kResultFalse:kResultOk;
    }
private:
    template<class S> void render(ProcessData& d,std::array<Lane,parameterCount>& lanes,std::size_t used) noexcept{
        S** input=nullptr;S** output=nullptr;int32 inputs=0,outputs=0;
        if(d.inputs&&d.numInputs>0){
            inputs=std::clamp(d.inputs[0].numChannels,0,2);
            if constexpr(std::is_same_v<S,float>)input=d.inputs[0].channelBuffers32;
            else input=d.inputs[0].channelBuffers64;
        }
        if(d.outputs&&d.numOutputs>0){
            outputs=std::clamp(d.outputs[0].numChannels,0,2);
            if constexpr(std::is_same_v<S,float>)output=d.outputs[0].channelBuffers32;
            else output=d.outputs[0].channelBuffers64;
        }
        uint64 silence=outputs==2?3u:outputs==1?1u:0u;
        for(int32 i=0;i<d.numSamples;++i){
            for(std::size_t k=0;k<used;++k)lanes[k].at(i,d.numSamples,engine_);
            const S gain=static_cast<S>(engine_.tick());
            for(int32 c=0;c<outputs;++c){
                if(!output||!output[c])continue;
                S x=input&&c<inputs&&input[c]?input[c][i]:S{0};
                if(!std::isfinite(x))x=0;
                const S y=gain==S{1}?x:x*gain;
                output[c][i]=y;if(y!=S{0})silence&=~(uint64{1}<<c);
            }
        }
        if(d.outputs&&d.numOutputs>0)d.outputs[0].silenceFlags=silence;
    }
    HostEngine engine_;
    double rate_{48000},bpm_{120};int32 n_{4},d_{4};
};

class Choice final:public StringListParameter{
public:
    Choice(const TChar* name,ParamID id,int32 flags):StringListParameter(name,id,nullptr,flags){}
    ParamValue toPlain(ParamValue v)const override{
        return std::round(std::clamp(std::isfinite(v)?v:0.,0.,1.)*getInfo().stepCount);
    }
    ParamValue toNormalized(ParamValue v)const override{
        const auto count=getInfo().stepCount;
        return count>0?std::round(std::clamp(std::isfinite(v)?v:0.,0.,double(count)))/count:0;
    }
};
class Controller final:public EditController{
public:
    static FUnknown* createInstance(void*){return static_cast<IEditController*>(new Controller);}
    tresult PLUGIN_API initialize(FUnknown* context) override{
        auto r=EditController::initialize(context);if(r!=kResultOk)return r;
        const TChar* names[]={STR16("START"),STR16("MID"),STR16("END"),STR16("LENGTH"),
            STR16("MID POSITION"),STR16("CURVE"),STR16("SHAPE"),STR16("WIDTH"),STR16("ATTACK"),
            STR16("RELEASE"),STR16("MIX"),STR16("ACTIVE"),STR16("MODE"),STR16("BYPASS")};
        for(std::size_t i=0;i<parameterCount;++i){
            const auto id=static_cast<ParamID>(100+i);
            if(steps[i]){
                int32 flags=ParameterInfo::kCanAutomate|ParameterInfo::kIsList;
                if(i==13)flags|=ParameterInfo::kIsBypass;
                auto* p=new Choice(names[i],id,flags);
                auto add=[&](const std::string& s){String128 value{};UString(value,128).fromAscii(s.c_str());p->appendString(value);};
                if(i<3){for(int x:divisions)add("1/"+std::to_string(x));}
                else if(i==3){add("1/4 Bar");add("1/2 Bar");for(int x=1;x<=64;++x)add(std::to_string(x)+(x==1?" Bar":" Bars"));}
                else if(i==6){for(auto s:{"RECT","SAW UP","SAW DOWN","TRIANGLE","SINE"})add(s);}
                else if(i==12){add("ONE SHOT");add("LOOP");}
                else{add("OFF");add("ON");}
                p->getInfo().defaultNormalizedValue=defaults[i];p->setNormalized(defaults[i]);parameters.addParameter(p);
            }else{
                const double lo=i==5?-100.:i==4?1.:0.;
                const double hi=i==4?99.:100.;
                parameters.addParameter(new RangeParameter(names[i],id,STR16("%"),lo,hi,lo+(hi-lo)*defaults[i]));
            }
        }
        return kResultOk;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id,ParamValue value) override{
        Parameters p;if(!p.set(static_cast<Param>(id),value))return kResultFalse;
        return EditController::setParamNormalized(id,p.get(static_cast<Param>(id)));
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) override{
        Parameters p;if(!decode(stream,p))return kResultFalse;
        for(std::size_t i=0;i<parameterCount;++i)EditController::setParamNormalized(
            static_cast<ParamID>(100+i),p.get(static_cast<Param>(100+i)));
        return kResultOk;
    }
};
}
BEGIN_FACTORY("125A Systems","","",Steinberg::PFactoryInfo::kNoFlags)
DEF_CLASS2(INLINE_UID_FROM_FUID(chopper::processorUID),Steinberg::PClassInfo::kManyInstances,
    kVstAudioEffectClass,"125A Chopperator",Steinberg::Vst::kDistributable,"Fx|Modulation",
    "0.6.0",kVstVersionString,chopper::Processor::createInstance)
DEF_CLASS2(INLINE_UID_FROM_FUID(chopper::controllerUID),Steinberg::PClassInfo::kManyInstances,
    kVstComponentControllerClass,"125A Chopperator Controller",0,"",
    "0.6.0",kVstVersionString,chopper::Controller::createInstance)
END_FACTORY
