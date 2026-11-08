"""Compile the actual explicit script-log callback with DIST macro removal."""
import importlib.util
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0,str(ROOT))

def load(filename,name):
    spec=importlib.util.spec_from_file_location(name,ROOT/"tools/tests"/filename)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
function=load("test_canvas_transparent_draw.py","log_callback_source").production_function
execute=load("test_host_font_selection.py","log_callback_compiler").execute_stock
PORTS=r"""
#define JCE_DIST 1
#include <jce/os/core/jce_log.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define LOG_TAG "runtime"
static int calls=0;
static bool permit=true;
static char result[4096];
#define REQUIRE_CODE(value,code) do { if(!(value)) { \
    fprintf(stderr,"runtime log failed %d: %s\n",__LINE__,#value); return code; \
} } while(false)
extern "C" void JCE_CALL jce_log_write(JceLogLevel level,const char *tag,
    const char *file,int line,const char *fmt,...)
{
    if(!permit)return;
    if(level!=JCE_LOG_LEVEL_INFO||strcmp(tag,"runtime")||!file||line<=0){calls=-100;return;}
    ++calls; va_list args;va_start(args,fmt);vsnprintf(result,sizeof result,fmt,args);va_end(args);
}
"""
MAIN=r"""
int main()
{
    LOG_INFO("internal","DIST must still remove internal chatter");
    REQUIRE_CODE(calls==0,1);
    rt_script_log(nullptr,"100% ready");
    REQUIRE_CODE(calls==1&&strcmp(result,"[script] 100% ready")==0,2);
    rt_script_log(nullptr,nullptr);
    REQUIRE_CODE(calls==2&&strcmp(result,"[script] ")==0,3);
    rt_script_log(nullptr,"file with spaces / %s / UTF8: \xe7\x9b\xae\xe5\xbd\x95");
    REQUIRE_CODE(calls==3&&strstr(result,"%s")!=nullptr&&strstr(result,"\xe7\x9b\xae\xe5\xbd\x95")!=nullptr,4);
    permit=false;rt_script_log(nullptr,"filtered");REQUIRE_CODE(calls==3,5);
    return 0;
}
"""

class RuntimeLogControls(unittest.TestCase):
    def test_actual_dist_callback_keeps_explicit_messages_and_runtime_policy(self):
        body=function((ROOT/"engine/src/application/jce_rt_script.c").read_text(encoding="utf-8"),"static void rt_script_log(")
        execute(self,PORTS+body+MAIN)
    def test_old_macro_suppression_fails_ordinary_runtime_control(self):
        body=function((ROOT/"engine/src/application/jce_rt_script.c").read_text(encoding="utf-8"),"static void rt_script_log(")
        current='jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,\n\t\t"[script] %s", msg ? msg : "");'
        self.assertEqual(body.count(current),1)
        old=body.replace(current,'LOG_INFO(LOG_TAG, "[script] %s", msg ? msg : "");')
        execute(self,PORTS+old+MAIN,negative=True,
            negative_exit=2,negative_marker="runtime log failed ")

if __name__=="__main__": unittest.main()
