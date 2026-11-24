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
#include "base/source/fobject.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "pluginterfaces/gui/iplugview.h"
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <cstdint>
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


void pumpGui(int ms){
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<end);
}
class GuiFrame final:public FObject, public IPlugFrame {
public:
    explicit GuiFrame(HWND parent):parent_(parent){}
    tresult PLUGIN_API resizeView(IPlugView* view,ViewRect* r) override{
        if(!view||!r)return kInvalidArgument;
        RECT outer{0,0,r->getWidth(),r->getHeight()};
        AdjustWindowRectEx(&outer,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_TOOLWINDOW);
        SetWindowPos(parent_,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        return view->onSize(r);
    }
    OBJ_METHODS(GuiFrame,FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IPlugFrame)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
private: HWND parent_;
};
class GuiHandler final:public FObject,public IComponentHandler{
public:
    std::array<int,14> begins{},edits{},ends{};
    std::array<double,14> last{};
    tresult PLUGIN_API beginEdit(ParamID id) override{if(id<100||id>113)return kInvalidArgument;++begins[id-100];return kResultOk;}
    tresult PLUGIN_API performEdit(ParamID id,ParamValue value) override{if(id<100||id>113||value<0||value>1)return kInvalidArgument;++edits[id-100];last[id-100]=value;return kResultOk;}
    tresult PLUGIN_API endEdit(ParamID id) override{if(id<100||id>113)return kInvalidArgument;++ends[id-100];return kResultOk;}
    tresult PLUGIN_API restartComponent(int32) override{return kResultOk;}
    OBJ_METHODS(GuiHandler,FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IComponentHandler)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};
void clickGui(HWND child,int x,int y){
    SendMessageW(child,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
    SendMessageW(child,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    SendMessageW(child,WM_LBUTTONUP,0,MAKELPARAM(x,y));pumpGui(60);
}
void dragGui(HWND child,int x,int y,int dy){
    SendMessageW(child,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
    SendMessageW(child,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    SendMessageW(child,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,y-dy));
    SendMessageW(child,WM_LBUTTONUP,0,MAKELPARAM(x,y-dy));pumpGui(60);
}
void wheelGui(HWND child,int x,int y){
    SendMessageW(child,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
    POINT pt{x,y};ClientToScreen(child,&pt);
    SendMessageW(child,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(pt.x,pt.y));
    pumpGui(100);
}
void guiContract(const VST3::Hosting::Module::Ptr& module,const std::filesystem::path& folder){
    Native n(module);auto handler=owned(new GuiHandler);
    require(n.controller->setComponentHandler(handler.get())==kResultOk,"gui handler");
    const auto inst=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=inst;wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"ChopperatorNativeTest";
    require(RegisterClassW(&wc)!=0,"gui host class");
    std::filesystem::create_directories(folder);
    for(int cycle=0;cycle<2;++cycle){
        auto view=owned(n.controller->createView(ViewType::kEditor));
        require(view.get()!=nullptr,"native editor present");
        require(view->isPlatformTypeSupported(kPlatformTypeHWND)==kResultTrue,"native HWND supported");
        ViewRect size{};require(view->getSize(&size)==kResultOk&&size.getWidth()==960&&size.getHeight()==576,"initial native editor dimensions");
        RECT outer{0,0,size.getWidth(),size.getHeight()};AdjustWindowRectEx(&outer,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_TOOLWINDOW);
        HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Chopperator editor QA",WS_OVERLAPPEDWINDOW,
            0,0,outer.right-outer.left,outer.bottom-outer.top,nullptr,nullptr,inst,nullptr);
        require(host!=nullptr,"gui host window");auto frame=owned(new GuiFrame(host));
        require(view->setFrame(frame.get())==kResultOk,"gui frame");
        require(view->attached(host,kPlatformTypeHWND)==kResultOk,"editor attach");
        ShowWindow(host,SW_SHOWNA);UpdateWindow(host);pumpGui(150);
        HWND child=GetWindow(host,GW_CHILD);require(child!=nullptr,"native editor child");
        FUnknownPtr<IParameterFinder> finder(view.get());
        require(finder.getInterface()!=nullptr,"native parameter finder");
        const std::array<std::array<int,3>,13> hits{{{{138,314,100}},{{366,314,101}},{{594,314,102}},{{822,314,103}},
            {{100,468,104}},{{252,468,105}},{{120,546,106}},{{404,468,107}},{{556,468,108}},{{708,468,109}},
            {{860,468,110}},{{610,34,111}},{{820,34,112}}}};
        for(const auto& hit:hits){ParamID id=0;require(finder->findParameter(hit[0],hit[1],id)==kResultTrue&&id==static_cast<ParamID>(hit[2]),"native parameter hit mapping");}
        require(n.controller->setParamNormalized(111,0)==kResultOk,"host active off");
        require(n.controller->setParamNormalized(106,0)==kResultOk,"host rect");
        require(n.controller->setParamNormalized(112,0)==kResultOk,"host one shot");pumpGui(80);
        clickGui(child,650,34);clickGui(child,920,34);clickGui(child,844,546);
        require(n.controller->getParamNormalized(111)==1&&n.controller->getParamNormalized(112)==1&&n.controller->getParamNormalized(106)==1,"native segmented controls");
        for(int id:{106,111,112})require(handler->begins[id-100]>0&&handler->edits[id-100]>0&&handler->ends[id-100]==handler->begins[id-100],"balanced edit notifications");
        require(n.controller->setParamNormalized(100,1./6)==kResultOk,"host start 1/2");
        require(n.controller->setParamNormalized(103,3./65)==kResultOk,"host length 2 bars");
        pumpGui(100);
        wheelGui(child,138,314);wheelGui(child,822,314);
        require(std::abs(n.controller->getParamNormalized(100)-2./6)<1e-8,"one wheel notch next note division");
        require(std::abs(n.controller->getParamNormalized(103)-4./65)<1e-8,"one wheel notch next length");
        pumpGui(700);
        // The owner has no mouse wheel: prove every LENGTH value can be
        // reached by left-button movement alone, then reverse the gesture.
        require(n.controller->setParamNormalized(103,0)==kResultOk,"host length minimum");
        pumpGui(80);
        SendMessageW(child,WM_MOUSEMOVE,0,MAKELPARAM(822,314));
        SendMessageW(child,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(822,314));
        for(int i=1;i<=65;++i){
            SendMessageW(child,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(822,314-3*i));
            require(std::abs(n.controller->getParamNormalized(103)-double(i)/65)<1e-8,"left drag reaches next length");
        }
        for(int i=64;i>=0;--i){
            SendMessageW(child,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(822,314-3*i));
            require(std::abs(n.controller->getParamNormalized(103)-double(i)/65)<1e-8,"left drag reverses every length");
        }
        SendMessageW(child,WM_LBUTTONUP,0,MAKELPARAM(822,314));pumpGui(80);
        require(n.controller->setParamNormalized(100,1./6)==kResultOk,"host start before drag");
        pumpGui(80);dragGui(child,138,314,25);
        require(std::abs(n.controller->getParamNormalized(100)-2./6)<1e-8,"left drag next note");
        require(n.controller->setParamNormalized(107,.5)==kResultOk,"host width before drag");
        pumpGui(80);dragGui(child,404,468,15);
        require(std::abs(n.controller->getParamNormalized(107)-.6)<1e-5,"left drag continuous width");
        std::cout<<"NATIVE_LEFT_DRAG_ALL_66_LENGTHS_PASS\n";
        // Actual editor resize negotiation; no claim of Studio Pro support.
        require(view->canResize()==kResultTrue,"editor resizable");
        ViewRect scaled{0,0,720,432};require(view->checkSizeConstraint(&scaled)==kResultOk,"resize constraint");
        require(scaled.getWidth()==720&&scaled.getHeight()==432,"aspect resize 75%");
        require(frame->resizeView(view.get(),&scaled)==kResultOk,"resize editor");pumpGui(100);
        require(view->getSize(&size)==kResultOk,"query resized geometry");
        RECT childRect{};require(GetClientRect(child,&childRect)!=0,"query resized HWND");
        std::cout<<"NATIVE_RESIZE actual="<<size.getWidth()<<"x"<<size.getHeight()<<" child="<<childRect.right<<"x"<<childRect.bottom<<"\n";
        require(size.getWidth()==720&&size.getHeight()==432&&childRect.right==720&&childRect.bottom==432,"actual resized geometry");
        ViewRect original{0,0,960,576};require(view->checkSizeConstraint(&original)==kResultOk&&frame->resizeView(view.get(),&original)==kResultOk,"resize restore");pumpGui(100);
        require(view->getSize(&size)==kResultOk&&size.getWidth()==960&&size.getHeight()==576,"restored geometry");
        require(GetClientRect(child,&childRect)!=0&&childRect.right==960&&childRect.bottom==576,"restored child geometry");
        // Optional actual screen evidence; capture failure is not silently called a screenshot.
        if(cycle==0){
            RECT r{};GetClientRect(child,&r);
            const int width=r.right,height=r.bottom;
            HDC screen=GetDC(child),dc=CreateCompatibleDC(screen);
            BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
            void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
            if(bitmap&&pixels&&width>0&&height>0){
                auto prev=SelectObject(dc,bitmap);BOOL copied=BitBlt(dc,0,0,width,height,screen,0,0,SRCCOPY);GdiFlush();
                auto* data=static_cast<std::uint32_t*>(pixels);
                std::size_t bright=0,orange=0;
                for(int i=0;i<width*height;++i){auto v=data[i];int r=(v>>16)&255,g=(v>>8)&255,b=v&255;
                    bright+=r>150&&g>150&&b>150;orange+=r>180&&g>70&&g<190&&b<110;}
                if(copied&&bright>100&&orange>100){
                    BITMAPFILEHEADER h{};h.bfType=0x4d42;h.bfOffBits=sizeof(h)+sizeof(BITMAPINFOHEADER);h.bfSize=h.bfOffBits+width*height*4;
                    std::ofstream f(folder/"native-editor.bmp",std::ios::binary);f.write(reinterpret_cast<char*>(&h),sizeof(h));f.write(reinterpret_cast<char*>(&bi.bmiHeader),sizeof(bi.bmiHeader));f.write(static_cast<char*>(pixels),width*height*4);
                    if(f)std::cout<<"NATIVE_EDITOR_CAPTURE "<<width<<"x"<<height<<"\n";
                }else std::cout<<"NATIVE_EDITOR_CAPTURE_UNAVAILABLE bright="<<bright<<" orange="<<orange<<"\n";
                SelectObject(dc,prev);
            }
            if(bitmap)DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(child,screen);
        }
        require(view->removed()==kResultOk&&view->setFrame(nullptr)==kResultOk,"editor removal");DestroyWindow(host);pumpGui(50);
    }
    require(n.controller->setComponentHandler(nullptr)==kResultOk,"clear handler");
    UnregisterClassW(wc.lpszClassName,inst);
    std::cout<<"NATIVE_EDITOR_CONTRACT_PASS two attach cycles, 13 hit mappings, three segment edits, discrete wheel steps, resize roundtrip\n";
}

int main(int argc,char** argv){
    require(argc==2||argc==3||argc==4,"bundle argument");
    struct ComScope{HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);~ComScope(){if(SUCCEEDED(hr))CoUninitialize();}} com;
    require(SUCCEEDED(com.hr),"COM apartment");
    std::string error;auto module=VST3::Hosting::Module::create(argv[1],error);
    if(!module){std::cerr<<error<<'\n';return 1;}
    HostApplication host;PluginContextFactory::instance().setPluginContext(&host);
    module->getFactory().setHostContext(&host);
    if(argc==4){if(std::string(argv[2])=="--editor"){guiContract(module,argv[3]);return 0;}require(std::string(argv[2])=="--programme","programme argument");programme(module,argv[3]);return 0;}
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
