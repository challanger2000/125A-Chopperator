#include "EditorPreferences.hpp"
#include <cstdlib>
#include <iostream>
using chopper::EditorPreferences;
static void check(bool ok,const char* what){if(!ok){std::cerr<<what<<'\n';std::exit(1);}}
int main(){
    for(bool ui:{false,true}){
        EditorPreferences p;auto& r=ui?p.uiWatches:p.watches;
        auto dispatch=[&]{if(ui)p.notifyUI();else p.notify();};
        int a=0,b=0,c=0;
        r.push_back({&a,[&]{++a;p.remove(&a);}});
        r.push_back({&b,[&]{++b;}});
        dispatch();check(a==1&&b==1&&r.size()==1,"self detach invalidates callback");
        dispatch();check(a==1&&b==2,"removed self invoked again");
        p.remove(&b);
        r.push_back({&a,[&]{++a;p.remove(&b);r.push_back({&c,[&]{++c;}});}});
        r.push_back({&b,[&]{++b;}});
        dispatch();check(a==2&&b==2&&c==0,"later detach or newly registered entry invoked");
        p.remove(&a);dispatch();check(c==1,"new entry absent next dispatch");
        p.remove(&c);
        r.push_back({&a,[&]{++a;p.remove(&a);dispatch();}});
        r.push_back({&b,[&]{++b;p.remove(&b);}});
        dispatch();check(a==3&&b==3&&r.size()==0,"nested dispatch invokes stale callback");
    }
    EditorPreferences p;int calls=0;int owner=0;
    p.uiWatches.push_back({&owner,[&]{++calls;}});
    p.markDirty=[&]{p.remove(&owner);};
    p.changeHelp(true);check(calls==0&&p.help,"dirty callback detach");
    std::cout<<"P009_REENTRANT_REGISTRY_PASS\n";
}
