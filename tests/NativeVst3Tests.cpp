#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/base/ustring.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
void require(bool ok,const char* why){if(!ok){std::cerr<<"FAIL "<<why<<'\n';std::exit(1);}}
void point(ParameterChanges& changes,ParamID id,int32 offset,double value){
    int32 qi=0,pi=0;auto* q=changes.addParameterData(id,qi);
    require(q&&q->addPoint(offset,value,pi)==kResultOk,"host fixture queue");
}
struct Native {
    std::unique_ptr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    FUnknownPtr<IAudioProcessor> processor;
    ProcessContext clock{};
    bool running=false;int32 channels=2;double rate=48000;
    Native(const VST3::Hosting::Module::Ptr& module){
        for(const auto& info:module->getFactory().classInfos()){
            if(info.category()==kVstAudioEffectClass){
                require(info.name()=="125A Chopperator","factory product name");
                provider=std::make_unique<PlugProvider>(module->getFactory(),info,true);break;
            }
        }
        require(bool(provider)&&provider->initialize(),"native provider initialize");
        component=provider->getComponentPtr();controller=provider->getControllerPtr();
        processor=FUnknownPtr<IAudioProcessor>(component.get());
        require(component.get()&&controller.get()&&processor.getInterface(),"native interfaces");
        require(controller->getParameterCount()==14,"parameter inventory");
        require(processor->getLatencySamples()==0&&processor->getTailSamples()==0,"latency and tail");
    }
    void start(double sr,int32 fmt,int32 ch){
        rate=sr;channels=ch;auto arrangement=ch==1?SpeakerArr::kMono:SpeakerArr::kStereo;
        require(processor->setBusArrangements(&arrangement,1,&arrangement,1)==kResultOk,"bus setup");
        require(component->activateBus(kAudio,kInput,0,true)==kResultOk,"activate input");
        require(component->activateBus(kAudio,kOutput,0,true)==kResultOk,"activate output");
        ProcessSetup s{};s.processMode=kRealtime;s.symbolicSampleSize=fmt;s.maxSamplesPerBlock=512;s.sampleRate=sr;
        require(processor->setupProcessing(s)==kResultOk,"processing setup");
        require(component->setActive(true)==kResultOk,"setActive");
        require(processor->setProcessing(true)==kResultOk,"setProcessing");
        running=true;clock.state=ProcessContext::kPlaying|ProcessContext::kTempoValid|ProcessContext::kTimeSigValid;
        clock.tempo=120;clock.timeSigNumerator=4;clock.timeSigDenominator=4;clock.sampleRate=sr;
    }
    void stop(){
        if(running){require(processor->setProcessing(false)==kResultOk,"processing stop");
            require(component->setActive(false)==kResultOk,"deactivate");running=false;}
    }
    ~Native(){stop();}
    void flush(ParameterChanges& changes,int32 fmt=kSample32){
        ProcessData d;d.symbolicSampleSize=fmt;d.inputParameterChanges=&changes;
        require(processor->process(d)==kResultOk,"zero-frame automation");changes.clearQueue();
    }
    template<class S> std::vector<S> render(int32 n,ParameterChanges* params=nullptr,bool haveClock=true){
        std::vector<S> l(static_cast<std::size_t>(n),S{1}),r(static_cast<std::size_t>(n),S{-.5});
        std::vector<S> ol(static_cast<std::size_t>(n),S{9}),orr(static_cast<std::size_t>(n),S{9});
        S* in[]{l.data(),r.data()};S* out[]{ol.data(),orr.data()};
        AudioBusBuffers i,o;i.numChannels=o.numChannels=channels;
        ProcessData d;d.numInputs=d.numOutputs=1;d.inputs=&i;d.outputs=&o;d.numSamples=n;
        d.inputParameterChanges=params;d.processContext=haveClock?&clock:nullptr;
        if constexpr(std::is_same_v<S,float>){d.symbolicSampleSize=kSample32;i.channelBuffers32=in;o.channelBuffers32=out;}
        else{d.symbolicSampleSize=kSample64;i.channelBuffers64=in;o.channelBuffers64=out;}
        require(processor->process(d)==kResultOk,"native process");
        bool silent=true;
        for(int32 k=0;k<n;++k){
            require(std::isfinite(ol[k])&&ol[k]>=0&&ol[k]<=1,"finite bounded PCM");
            if(channels==2)require(orr[k]==S{-.5}*ol[k],"shared stereo gain");
            silent&=ol[k]==0;
        }
        require(o.silenceFlags==(silent?(channels==2?3u:1u):0u),"actual output silence flags");
        if(params)params->clearQueue();
        clock.projectTimeSamples+=1000000; // deliberately unrelated project position
        return ol;
    }
    std::vector<char> save(){
        MemoryStream stream;require(component->getState(&stream)==kResultOk,"component save");
        const auto n=stream.getSize();require(n==120,"state size");
        return std::vector<char>(stream.getData(),stream.getData()+n);
    }
    bool load(std::vector<char>& bytes){
        MemoryStream stream(bytes.data(),static_cast<TSize>(bytes.size()));
        return component->setState(&stream)==kResultOk;
    }
};
template<class S> void timing(const VST3::Hosting::Module::Ptr& module,double sr,int32 ch){
    Native n(module);const int32 fmt=std::is_same_v<S,float>?kSample32:kSample64;n.start(sr,fmt,ch);
    ParameterChanges p(14);
    point(p,107,0,0);point(p,108,0,0);point(p,109,0,0);n.flush(p,fmt);
    const int edge=static_cast<int>(sr*.0005);
    const int on=64,off=256;
    point(p,111,on,1);point(p,111,off,0);
    const auto pcm=n.render<S>(512,&p);
    for(int i=0;i<512;++i){
        const double expected=i<on?1:i<off?std::max(0.,1.-double(i-on+1)/edge):std::min(1.,double(i-off+1)/edge);
        require(std::abs(double(pcm[i])-expected)<1e-6,"sample-exact ACTIVE and de-click slope");
    }
    // Continuous host automation from the implicit previous point at -1.
    point(p,110,0,0);point(p,111,0,1);n.flush(p,fmt);
    n.render<S>(512);
    point(p,110,255,1);
    const auto ramp=n.render<S>(256,&p,false);
    for(int i=0;i<256;++i)require(std::abs(double(ramp[i])-(1-double(i+1)/256))<1e-6,"continuous MIX automation");
    // Actual compiled plugin returns dry in bypass with a warm oscillator.
    point(p,113,0,1);n.flush(p,fmt);
    const auto dry=n.render<S>(512);
    for(int i=edge;i<512;++i)require(dry[i]==S{1},"settled exact bypass");
}
std::vector<float> trajectory(Native& n){
    std::vector<float> pcm;
    for(int i=0;i<25024;){const int count=std::min(73+i%311,25024-i);auto a=n.render<float>(count,nullptr,i%3!=0);pcm.insert(pcm.end(),a.begin(),a.end());i+=count;}
    return pcm;
}
int main(int argc,char** argv){
    require(argc==2,"bundle argument");
    std::string error;auto module=VST3::Hosting::Module::create(argv[1],error);
    if(!module){std::cerr<<error<<'\n';return 1;}
    HostApplication host;PluginContextFactory::instance().setPluginContext(&host);
    module->getFactory().setHostContext(&host);
    timing<float>(module,48000,2);timing<double>(module,48000,1);
    timing<float>(module,96000,1);timing<double>(module,192000,2);
    std::cout<<"NATIVE_ACTIVE_MIX_BYPASS_4X_PASS\n";
    Native n(module);n.start(48000,kSample32,2);
    ParameterChanges p(14);
    point(p,103,0,0);point(p,108,0,0);point(p,109,0,0);point(p,111,0,1);
    n.flush(p);
    auto state=n.save();
    const auto pcm=trajectory(n);
    int open=0,closed=0;
    for(int i=0;i<24000;++i){
        const double x=double(i)/24000;
        const double integral=x<=.5?2*x+14*x*x:4.5+16*(x-.5)-14*(x-.5)*(x-.5);
        const double cycles=.25*integral,phase=cycles-std::floor(cycles);
        if(phase>.01&&phase<.49){require(pcm[i]==1,"trajectory open");++open;}
        if(phase>.51&&phase<.99){require(pcm[i]==0,"trajectory closed");++closed;}
    }
    require(open>1000&&closed>1000,"trajectory nontrivial");
    for(int i=24024;i<25024;++i)require(pcm[i]==1,"one-shot completion dry");
    n.stop();
    require(n.load(state),"restore saved valid state");
    for(std::size_t size=0;size<state.size();++size){
        std::vector<char> bad(state.begin(),state.begin()+static_cast<std::ptrdiff_t>(size));
        require(!n.load(bad)&&n.save()==state,"truncated state rollback");
    }
    auto invalid=state;const double nan=std::numeric_limits<double>::quiet_NaN();
    std::memcpy(invalid.data()+8,&nan,sizeof(nan));
    require(!n.load(invalid)&&n.save()==state,"nonfinite state rollback");
    Native fresh(module);require(fresh.load(state),"fresh state load");
    MemoryStream controllerState(state.data(),static_cast<TSize>(state.size()));
    require(fresh.controller->setComponentState(&controllerState)==kResultOk,"controller component state");
    require(fresh.controller->getParamNormalized(103)==0&&fresh.controller->getParamNormalized(111)==1,"controller recall");
    String128 name{};char text[128]{};
    require(fresh.controller->getParamStringByValue(103,1,name)==kResultOk,"length display");
    UString(name,128).toAscii(text,128);require(std::string(text)=="64 Bars","length endpoint text");
    fresh.start(48000,kSample32,2);
    require(trajectory(fresh)==pcm,"fresh native instance exact recall PCM");
    fresh.stop();std::cout<<"NATIVE_TRAJECTORY_STATE_RECALL_PASS open="<<open<<" closed="<<closed<<"\n";
}
