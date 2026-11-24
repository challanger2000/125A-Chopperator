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
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
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

constexpr double pi=3.1415926535897932384626433832795;
struct SignalBlock {
    std::array<double,512> l{},r{},ol{},orr{};
    double* in[2]{l.data(),r.data()},*out[2]{ol.data(),orr.data()};
    AudioBusBuffers i,o;
    ProcessData data;
    SignalBlock(Native& n,int32 count){
        i.numChannels=o.numChannels=2;i.channelBuffers64=in;o.channelBuffers64=out;
        data.numInputs=data.numOutputs=1;data.inputs=&i;data.outputs=&o;
        data.numSamples=count;data.symbolicSampleSize=kSample64;data.processContext=&n.clock;
    }
    void process(Native& n){require(n.processor->process(data)==kResultOk,"quality native process");}
};
void setConstant(Native& n,int divisionIndex,int shapeIndex,double width,double attack,double release){
    ParameterChanges p(14);
    for(auto id:{100u,101u,102u})point(p,id,0,double(divisionIndex)/6);
    point(p,103,0,65./65);point(p,106,0,double(shapeIndex)/4);
    point(p,107,0,width);point(p,108,0,attack);point(p,109,0,release);
    point(p,110,0,1);point(p,111,0,1);point(p,112,0,1);
    n.flush(p,kSample64);
}
std::vector<double> tone(Native& n,double hz,int seconds=1){
    const int sr=static_cast<int>(n.rate),frames=sr*seconds;
    std::vector<double> result;result.reserve(frames);
    SignalBlock b(n,256);
    // Warm one second to exclude activation transient. All selected tones and
    // modulation frequencies are integer-Hz, so the window is coherent.
    for(int i=-sr;i<frames;){
        const int count=std::min(256,frames-i);b.data.numSamples=count;
        for(int k=0;k<count;++k){
            b.l[k]=.5*std::sin(2*pi*hz*double(i+k)/sr);
            b.r[k]=-.5*b.l[k];
        }
        b.process(n);
        for(int k=0;k<count;++k){
            require(std::isfinite(b.ol[k])&&std::abs(b.ol[k])<=std::abs(b.l[k])+1e-14,"no audio boost");
            require(b.orr[k]==-.5*b.ol[k],"tone stereo gain");
            if(i+k>=0)result.push_back(b.ol[k]);
        }
        i+=count;
    }
    require(result.size()==static_cast<std::size_t>(frames),"coherent capture");
    return result;
}
double amplitude(const std::vector<double>& x,double hz,double sr){
    double re=0,im=0;
    for(std::size_t k=0;k<x.size();++k){
        const double a=2*pi*hz*double(k)/sr;
        re+=x[k]*std::cos(a);im+=x[k]*std::sin(a);
    }
    return 2*std::hypot(re,im)/double(x.size());
}
void spectral(const VST3::Hosting::Module::Ptr& module,bool negative){
    Native n(module);n.start(48000,kSample64,2);
    // Sinusoidal AM: .5 input amplitude, g=.5-.5*cos(2*pi*8*t).
    // Analytic carrier .25, two sidebands .125, no other tones.
    setConstant(n,4,4,1,0,0);
    const auto x=tone(n,1000);
    const double c=amplitude(x,1000,48000),lo=amplitude(x,992,48000),hi=amplitude(x,1008,48000);
    double error=0;
    for(std::size_t i=0;i<x.size();++i){
        const double t=double(i)/48000;
        const double expected=.5*std::sin(2*pi*1000*t)*(.5-.5*std::cos(2*pi*8*t));
        error+=(x[i]-expected)*(x[i]-expected);
    }
    error=std::sqrt(error/double(x.size()));
    require(std::abs(c-(negative?.5:.25))<1e-6,"SINE carrier oracle");
    require(std::abs(lo-.125)<1e-6&&std::abs(hi-.125)<1e-6&&error<1e-6,"SINE sideband and residual oracle");
    std::cout<<"SINE_AM carrier="<<c<<" lower="<<lo<<" upper="<<hi<<" residual_rms="<<error<<"\n";
    n.stop();
    Native q(module);q.start(48000,kSample64,2);q.clock.tempo=240;
    setConstant(q,6,0,.5,0,0);
    const auto y=tone(q,23000);
    // Upper 17th 64-Hz sideband would be 24088 Hz, folds to 23912 Hz.
    // Reference for an ideal hard 50% duty pulse: coefficient 1/(17*pi).
    // For a 0.5 ms linear slew (32/1000 of period), multiply by sinc.
    const double hard=1/(17*pi);
    const double z=pi*17*.0005*64;
    const double expected=hard*std::abs(std::sin(z)/z);
    const double actual=amplitude(y,23912,48000)/.5;
    require(actual<hard&&std::abs(actual-expected)<.002,"RECT folded sideband vs trapezoid oracle");
    std::cout<<"RECT_64HZ_FOLDED_SIDEBAND frequency=23912 relative_dBc="<<20*std::log10(actual)
             <<" hard_dBc="<<20*std::log10(hard)<<" continuous_reference_dBc="<<20*std::log10(expected)<<"\n";
    // This is one isolated fold, not a full alias-power or audibility metric.
}
void waveformContracts(const VST3::Hosting::Module::Ptr& module){
    for(int shape=0;shape<5;++shape){
        Native n(module);n.start(48000,kSample64,2);
        setConstant(n,2,shape,1,0,0); // quarter notes, 2 Hz, width full
        std::vector<double> x;
        for(int i=0;i<48000;){int count=std::min(512,48000-i);auto a=n.render<double>(count);x.insert(x.end(),a.begin(),a.end());i+=count;}
        for(int k=24000+100;k<48000-100;++k){
            const double u=double(k-24000)/24000;
            double expected=0;
            switch(shape){case 0:expected=1;break;case 1:expected=u;break;case 2:expected=1-u;break;
                case 3:expected=1-std::abs(2*u-1);break;case 4:expected=.5-.5*std::cos(2*pi*u);break;}
            require(std::abs(x[k]-expected)<1e-7,"native waveform oracle");
        }
    }
    std::cout<<"NATIVE_ALL_FIVE_SHAPES_PASS\n";
}
void realtimeProbe(const VST3::Hosting::Module::Ptr& module){
    using Clock=std::chrono::steady_clock;
    Native n(module);n.start(48000,kSample64,2);
    setConstant(n,6,4,.5,0,0);
    SignalBlock b(n,128);
    for(int i=0;i<128;++i){b.l[i]=.25*std::sin(2*pi*440*i/48000);b.r[i]=-.5*b.l[i];}
    ParameterChanges p(14);
    point(p,100,0,0);point(p,101,0,1);point(p,102,0,0);
    point(p,104,0,.01);point(p,105,0,1);n.flush(p,kSample64);
    std::array<double,384> micros{};
    const auto interval=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(128./48000));
    auto deadline=Clock::now();double sum=0;int over=0;
    for(std::size_t i=0;i<micros.size();++i){
        // Automation setup is host work, intentionally outside callback measurement.
        p.clearQueue();point(p,104,127,i%2?.01:.99);point(p,105,127,i%2?0.:1.);
        point(p,107,127,i%2?.2:.9);b.data.inputParameterChanges=&p;
        const auto begin=Clock::now();
        const auto status=n.processor->process(b.data);
        const auto end=Clock::now();
        require(status==kResultOk,"paced native automation");
        const double us=std::chrono::duration<double,std::micro>(end-begin).count();
        micros[i]=us;sum+=us;over+=us>128./48000*1e6;
        for(int k=0;k<128;++k)require(std::isfinite(b.ol[k])&&std::abs(b.ol[k])<=std::abs(b.l[k])+1e-14,"paced bounded output");
        deadline+=interval;std::this_thread::sleep_until(deadline);
    }
    std::sort(micros.begin(),micros.end());
    std::cout<<"PACED_NATIVE_MEASUREMENT callbacks="<<micros.size()<<" mean_us="<<sum/micros.size()
        <<" p95_us="<<micros[364]<<" p99_us="<<micros[380]<<" max_us="<<micros.back()
        <<" deadline_us="<<128./48000*1e6<<" callback_overruns="<<over<<"\n";
    // Cloud scheduler and physical ASIO are different. Observation only.
}


void programme(const VST3::Hosting::Module::Ptr& module,const std::filesystem::path& folder){
    for(const std::string name:{"guitar_e2","guitar_b3","pad_c4"}){
        std::ifstream in(folder/(name+".f64le"),std::ios::binary|std::ios::ate);
        require(bool(in),"programme fixture open");
        const auto bytes=static_cast<std::streamoff>(in.tellg());
        require(bytes>=48000*16&&bytes%16==0,"programme size");
        const int frames=static_cast<int>(std::min<std::streamoff>(bytes/16,192000));
        std::vector<double> source(static_cast<std::size_t>(frames)*2);
        in.seekg(0);in.read(reinterpret_cast<char*>(source.data()),static_cast<std::streamsize>(source.size()*sizeof(double)));
        require(bool(in),"programme exact read");
        for(const int shape:{0,4}){
            Native n(module);n.start(48000,kSample64,2);
            setConstant(n,4,shape,shape==0?.5:1.,0,0);
            SignalBlock b(n,256);
            std::ofstream out(folder/(name+(shape==0?"_rect":"_sine")+".f64le"),std::ios::binary);
            require(bool(out),"programme output open");
            double dryPower=0,wetPower=0,deltaPower=0,errorPower=0,referencePower=0;
            int checked=0;
            for(int i=0;i<frames;){
                const int count=std::min(256,frames-i);b.data.numSamples=count;
                for(int k=0;k<count;++k){b.l[k]=source[2*(i+k)];b.r[k]=source[2*(i+k)+1];}
                b.process(n);
                for(int k=0;k<count;++k){
                    const int j=i+k,phase=j%6000;
                    // Independent steady-state analytic envelope. Exclude minimum-slew
                    // transition samples for rectangular shape; quantify separately
                    // against the slope bound. Sine starts at zero after activation.
                    const bool oracle=j>=6000&&(shape==4||(phase>=32&&phase<3000)||(phase>=3032));
                    const double g=shape==0?(phase<3000?1.:0.):.5-.5*std::cos(2*pi*phase/6000.);
                    for(int c=0;c<2;++c){
                        const double x=source[2*j+c],y=c==0?b.ol[k]:b.orr[k];
                        require(std::isfinite(x)&&std::isfinite(y)&&std::abs(y)<=std::abs(x)+1e-13,"programme finite no boost");
                        require(x*y>=0,"programme polarity");
                        dryPower+=x*x;wetPower+=y*y;deltaPower+=(x-y)*(x-y);
                        if(oracle){
                            const double ref=x*g;
                            errorPower+=(y-ref)*(y-ref);referencePower+=x*x;++checked;
                        }
                        out.write(reinterpret_cast<const char*>(&y),sizeof(y));
                    }
                }
                i+=count;
            }
            require(bool(out)&&checked>10000&&referencePower>1e-5&&dryPower>1e-5,"programme meaningful signal");
            require(wetPower>1e-6&&wetPower<.9*dryPower&&deltaPower>.01*dryPower,"programme audible modulation not constant dry or mute");
            const double relative=std::sqrt(errorPower/referencePower);
            require(relative<1e-8,"programme independent PCM envelope");
            std::cout<<"PROGRAMME_PASS "<<name<<" shape="<<shape<<" frames="<<frames
                     <<" relative_error="<<relative<<" wet_dB="<<10*std::log10(wetPower/dryPower)<<"\n";
            // Same fixture through settled hard dry on a separate native instance.
            Native dry(module);dry.start(48000,kSample64,2);
            ParameterChanges p(14);point(p,110,0,0);dry.flush(p,kSample64);
            SignalBlock d(dry,256);
            for(int i=0;i<frames;){
                const int count=std::min(256,frames-i);d.data.numSamples=count;
                for(int k=0;k<count;++k){d.l[k]=source[2*(i+k)];d.r[k]=source[2*(i+k)+1];}
                d.process(dry);
                for(int k=24;k<count;++k)require(d.ol[k]==d.l[k]&&d.orr[k]==d.r[k],"programme settled exact dry");
                i+=count;
            }
        }
    }
}

int main(int argc,char** argv){
    require(argc==2||argc==3||argc==4,"bundle argument");
    std::string error;auto module=VST3::Hosting::Module::create(argv[1],error);
    if(!module){std::cerr<<error<<'\n';return 1;}
    HostApplication host;PluginContextFactory::instance().setPluginContext(&host);
    module->getFactory().setHostContext(&host);
    if(argc==4){require(std::string(argv[2])=="--programme","programme argument");programme(module,argv[3]);return 0;}
    if(argc==3){require(std::string(argv[2])=="--negative-spectrum","negative control argument");spectral(module,true);return 2;}
    waveformContracts(module);spectral(module,false);realtimeProbe(module);
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
