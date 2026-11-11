"""Exact installed-font and stock-source owners with counted CPU ports.

These controls verify selection, bounded lookup and buffer ownership. They do
not certify font rendering, physical CJK coverage or platform font installations.
"""
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/tests" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SOURCE = load("test_canvas_transparent_draw.py", "host_font_source")
C_RUN = load("test_material_owned_cleanup.py", "host_font_c_runner")


class NegativeRuntime:
    def __init__(self, test):
        self.test, self.phase = test, 0

    def assertEqual(self, actual, expected, message):
        self.phase += 1
        if self.phase == 1:
            self.test.assertEqual(actual, expected, message)
        else:
            self.test.assertNotEqual(actual, expected, message)

    def skipTest(self, message):
        self.test.skipTest(message)


def cleanup_stock_directory(temporary, base, *, windows, pause=time.sleep):
    """Bound transient Windows deletion refusal after the owned probe has joined."""
    base = Path(base).resolve(strict=True)
    delays = (0.05, 0.1, 0.2, 0.4)
    for attempt in range(len(delays) + 1):
        directory = Path(temporary.name)
        if directory.is_symlink() or directory.resolve().parent != base:
            raise ValueError("Stock probe cleanup escaped its fixture directory")
        try:
            temporary.cleanup()
            return
        except PermissionError as error:
            if (not windows or getattr(error, "winerror", None) not in (5, 32, 33)
                    or attempt == len(delays)):
                raise
            pause(delays[attempt])


class StockProbeDirectory(tempfile.TemporaryDirectory):
    """Retain strict failures while allowing only bounded image-release delay."""
    def __init__(self, base):
        self.fixture_base = Path(base).resolve(strict=True)
        super().__init__(dir=self.fixture_base)

    def __exit__(self, kind, error, traceback):
        try:
            cleanup_stock_directory(self, self.fixture_base, windows=os.name == "nt")
        except Exception as cleanup_error:
            if error is not None:
                raise error.with_traceback(traceback) from cleanup_error
            raise


def execute_stock(test, code, negative=False, *, negative_exit=None, negative_marker=None):
    """Shared C++ compilation with an explicit ordinary runtime-failure witness."""
    if negative:
        test.assertTrue(type(negative_exit) is int and 1 <= negative_exit <= 125,
            "negative requires an explicit ordinary exit code, excluding signal/exception statuses")
        test.assertTrue(isinstance(negative_marker, str) and bool(negative_marker),
            "negative requires its actual line-numbered stderr assertion marker")
    else:
        test.assertIsNone(negative_exit)
        test.assertIsNone(negative_marker)
    spec = importlib.util.spec_from_file_location("host_font_build_driver", ROOT / "tools/build/jce.py")
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    includes = ROOT / "engine/include"
    if driver.HOST == "windows":
        env = driver.msvc_env(driver.resolve_target(None))
        compiler = ["cl", "/nologo", "/TP", "/std:c++20", "/EHsc", "/W3", "/I" + str(includes)]
    else:
        cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            test.skipTest("No C++ compiler for exact stock font controls")
        env, compiler = None, [cxx, "-std=c++20", "-I" + str(includes)]
    base = ROOT / "build/tests/host-font-selection-tests"
    base.mkdir(parents=True, exist_ok=True)
    with StockProbeDirectory(base) as temporary:
        directory = Path(temporary)
        source = directory / "probe.cpp"
        source.write_text(code, encoding="utf-8", newline="\n")
        executable = directory / ("probe.exe" if driver.HOST == "windows" else "probe")
        command = compiler + [str(source)] + (["/Fe:probe.exe", "/Fo:probe.obj"]
            if driver.HOST == "windows" else ["-o", str(executable)])
        result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
        if negative:
            # Every consumer declares its fixture's known ordinary status.
            # Aborts, signals or Windows exception statuses cannot certify it.
            test.assertEqual(result.returncode, negative_exit, result.stdout + result.stderr)
            test.assertRegex(result.stderr,
                r"(?m)^" + re.escape(negative_marker) + r"[0-9]+: .+")
        else:
            test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout


def host_probe(mutation=None, *, symbols=False):
    source = (ROOT / "engine/src/os/platform/jce_host_font.c").read_text(encoding="utf-8")
    owner = source[source.index("static bool host_font_copy("):]
    if mutation == "old_print":
        old = "if (role == JCE_HOST_FONT_UI_LATIN && host_font_ink_free(dst, cap, true)) return true;"
        assert owner.count(old) == 1
        owner = owner.replace(old, "if (role == JCE_HOST_FONT_UI_LATIN && host_font_print(false, dst, cap, true)) return true;", 1)
    elif mutation == "cjk":
        assert owner.count("if (!supported) return false;") == 1
        owner = owner.replace("if (!supported) return false;", "(void)supported;", 1)
    elif mutation == "skip_ink":
        old = "if (role == JCE_HOST_FONT_UI_LATIN && host_font_ink_free(dst, cap, true)) return true;"
        assert owner.count(old) == 1
        owner = owner.replace(old, "/* mutation: old system-Sans-only default */", 1)
    elif mutation == "wrong_bold":
        old = 'bold ? "segoeprb" : "segoepr"'
        assert owner.count(old) == 1
        owner = owner.replace(old, 'bold ? "segoepr" : "segoepr"', 1)
    elif mutation == "fc_family":
        old = "!host_font_name_matches(resolved_family, family)"
        assert owner.count(old) == 1
        owner = owner.replace(old, "false", 1)
    elif mutation == "fc_style":
        old = "!host_font_name_matches(resolved_style, style)"
        assert owner.count(old) == 1
        owner = owner.replace(old, "false", 1)
    elif mutation == "skip_symbols":
        old = "if (role == JCE_HOST_FONT_UI_SYMBOLS) return host_font_symbols(dst, cap);"
        assert owner.count(old) == 1
        owner = owner.replace(old, "/* mutation: old Latin/CJK-only roles */", 1)
    return HOST_PORTS + owner + (SYMBOL_CONTROL if symbols else HOST_CONTROL)


def recoverable_owner(header):
    return SOURCE.production_function(header,
        "template <typename AddFont>\nstatic inline auto ed_font_transfer_recoverable(")


def recovery_probe(negative=False):
    """Actual tiny-source and scoped owner, with an ordinary counted-user-error port."""
    header = (ROOT / "editor/src/ui/jce_editor_font_data.h").read_text(encoding="utf-8")
    owners = "\n".join((
        SOURCE.production_function(header, "static inline void *ed_font_adopt_host_buffer("),
        SOURCE.production_function(header, "template <typename AddFont>"),
        recoverable_owner(header)))
    if negative:
        marker = "    assert_enabled = false;"
        assert owners.count(marker) == 1
        owners = owners.replace(marker, "    (void)assert_enabled;", 1)
    return RECOVERY_PORTS + owners + RECOVERY_CONTROL


def stock_probe(negative=False):
    source = (ROOT / "editor/src/ui/jce_editor_style.cpp").read_text(encoding="utf-8")
    header = (ROOT / "editor/src/ui/jce_editor_font_data.h").read_text(encoding="utf-8")
    owner = SOURCE.production_function(source, "static ImFont *load_stock_font(")
    if negative:
        assert owner.count("        ED_FREE(data);") == 1
        owner = owner.replace("        ED_FREE(data);", "        (void)data;", 1)
    transfer = SOURCE.production_function(header, "template <typename AddFont>")
    return STOCK_PORTS + transfer + "\n" + recoverable_owner(header) + "\n" + owner + STOCK_CONTROL



def owning_transfer_probe(negative=False):
    """Actual helper + consuming typed port, not an ImGui parser replacement.

    The native font_data target separately anchors real ImGui parser rollback.
    The old helper's extra free is counted and refused before a real doublefree.
    """
    header = (ROOT / "editor/src/ui/jce_editor_font_data.h").read_text(encoding="utf-8")
    transfer = SOURCE.production_function(header, "template <typename AddFont>")
    if negative:
        marker = "    return add(data, size);"
        assert transfer.count(marker) == 1
        transfer = transfer.replace(marker,
            "    auto font = add(data, size);\n"
            "    if (!font) ED_FREE(data);\n"
            "    return font;", 1)
    return OWNING_TRANSFER_PORTS + transfer + "\n" + OWNING_TRANSFER_CONTROL


RECOVERY_PORTS = r'''
#include <initializer_list>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <cstdint>
#define REQUIRE(value) do{if(!(value)){fprintf(stderr,"recoverable font owner failed %d: %s\n",__LINE__,#value);return 3;}}while(0)
static unsigned frees,calls;static bool assert_enabled=true,inside_disabled;
static void jce_free(void *data){if(data){++frees;std::free(data);}}
struct Font{};
static Font *counted_owning_add(void *data,int bytes,bool fail){
    ++calls;inside_disabled=!assert_enabled;
    jce_free(data);static Font font;return fail?nullptr:&font;
}
'''
RECOVERY_CONTROL = r'''
int main(void){
    for(unsigned bytes: {1u,100u}){
        unsigned before=frees;void *raw=std::malloc(bytes);REQUIRE(raw);
        REQUIRE(!ed_font_adopt_host_buffer(raw,bytes));
        REQUIRE(frees==before+1&&!calls&&assert_enabled);
    }
    void *raw=std::malloc(128);REQUIRE(raw);unsigned before=frees;
    REQUIRE(ed_font_transfer_recoverable(raw,128,assert_enabled,[](void *data,int bytes){return counted_owning_add(data,bytes,false);}));
    REQUIRE(inside_disabled&&assert_enabled&&calls==1&&frees==before+1);
    raw=std::malloc(128);REQUIRE(raw);before=frees;
    REQUIRE(!ed_font_transfer_recoverable(raw,128,assert_enabled,[](void *data,int bytes){return counted_owning_add(data,bytes,true);}));
    REQUIRE(inside_disabled&&assert_enabled&&calls==2&&frees==before+1);
    assert_enabled=false;raw=std::malloc(128);REQUIRE(raw);before=frees;
    REQUIRE(!ed_font_transfer_recoverable(raw,128,assert_enabled,[](void *data,int bytes){return counted_owning_add(data,bytes,true);}));
    REQUIRE(inside_disabled&&!assert_enabled&&calls==3&&frees==before+1);
    return 0;
}
'''


OWNING_TRANSFER_PORTS = r'''
#include <cstdlib>
#include <cstdio>
#include <cstring>
#define REQUIRE(value) do{if(!(value)){fprintf(stderr,"owning font transfer failed %d: %s\n",__LINE__,#value);return 3;}}while(0)
static void *tracked, *retained;
static unsigned release_calls, duplicate_releases;
static bool released;
static void counted_free(void *data){
    if(data!=tracked)std::abort();
    ++release_calls;
    if(released){++duplicate_releases;return;} // Refuse actual duplicate free.
    released=true;std::free(data);
}
#define ED_FREE counted_free
struct Font{};
static Font *owning_add(void *data,int bytes,bool reject){
    if(data!=tracked||bytes!=128||released)std::abort();
    if(reject){counted_free(data);return nullptr;}
    retained=data;static Font font;return &font;
}
'''
OWNING_TRANSFER_CONTROL = r'''
int main(void){
    tracked=std::malloc(128);REQUIRE(tracked);std::memset(tracked,0x41,128);
    REQUIRE(!ed_font_transfer(tracked,128,[](void *data,int bytes){return owning_add(data,bytes,true);}));
    REQUIRE(release_calls==1&&duplicate_releases==0&&released&&!retained);
    tracked=std::malloc(128);REQUIRE(tracked);std::memset(tracked,0x52,128);
    release_calls=duplicate_releases=0;released=false;
    REQUIRE(ed_font_transfer(tracked,128,[](void *data,int bytes){return owning_add(data,bytes,false);}));
    REQUIRE(!release_calls&&!released&&retained==tracked&&static_cast<unsigned char *>(retained)[127]==0x52);
    counted_free(retained);retained=nullptr;
    REQUIRE(release_calls==1&&duplicate_releases==0&&released);
    return 0;
}
'''


def editor_override_probe(negative=False):
    """Actual style/host-role/buffer owners; typed atlas admission, not a parser."""
    style = (ROOT / "editor/src/ui/jce_editor_style.cpp").read_text(encoding="utf-8")
    data = (ROOT / "editor/src/ui/jce_editor_font_data.h").read_text(encoding="utf-8")
    host = (ROOT / "engine/src/os/platform/jce_host_font.c").read_text(encoding="utf-8")
    host = host[host.index("static bool host_font_copy("):]
    owners = "\n".join(SOURCE.production_function(style, signature) for signature in (
        "static bool file_exists_readable(", "static void *read_font_file(",
        "static bool find_system_font(", "static bool resolve_font_path(",
        "static ImFont *load_font_with_fallback(", "static ImFont *load_editor_cjk_font("))
    if negative:
        marker = "        if (family < 2u) {"
        assert owners.count(marker) == 1
        owners = owners.replace(marker, "        if (is_override) return NULL;\n" + marker, 1)
    buffers = "\n".join(SOURCE.production_function(data, signature) for signature in (
        "static inline void *ed_font_adopt_host_buffer(",
        "static inline void *ed_font_read_host(", "template <typename AddFont>"))
    return HOST_PORTS + host + EDITOR_OVERRIDE_PORTS + buffers + recoverable_owner(data) + owners + EDITOR_OVERRIDE_CONTROL


EDITOR_OVERRIDE_PORTS = r'''
#undef LOG_WARN
#define JCE_DIST 1
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_alloc.h>
#include <climits>
#define LOG_TAG "editor_style"
static const char *authored="/authored/user.ttf",*ink="D:/OS/Fonts/inkfree.ttf",*cjk_face="D:/OS/Fonts/msyh.ttf";
static const char *kaiti="D:/OS/Fonts/simkai.ttf";
static unsigned style_warnings,style_infos,reads,adds,allocations,frees;
static int fail_mode;
static char style_message[1024],style_info[1024],read_paths[16][1024];
static void *retained;
void JCE_CALL jce_log_write(JceLogLevel level,const char *tag,const char *,int,const char *format,...){
    if(strcmp(tag,LOG_TAG))abort();
    char *message=nullptr;
    if(level==JCE_LOG_LEVEL_WARN){++style_warnings;message=style_message;}
    else if(level==JCE_LOG_LEVEL_INFO){++style_infos;message=style_info;}
    else abort();
    va_list args;va_start(args,format);vsnprintf(message,1024,format,args);va_end(args);
}
size_t JCE_CALL jce_strlcpy(char *dst,const char *src,size_t cap){
    size_t size=strlen(src);if(cap){size_t n=size<cap?size:cap-1;memcpy(dst,src,n);dst[n]=0;}return size;
}
void jce_free(void *data){if(data){++frees;free(data);}}
#define ED_FREE jce_free
void *jce_fs_host_read_all(const char *path,uint64_t *size){
    if(reads>=16)abort();strcpy(read_paths[reads++],path);*size=0;
    if((fail_mode==1&&!strcmp(path,authored))||(fail_mode==4&&!strcmp(path,ink))||
       (fail_mode==6&&!strcmp(path,kaiti)))return nullptr;
    const size_t bytes=(fail_mode==5&&!strcmp(path,authored))?100:128;
    void *data=malloc(bytes);if(!data)abort();++allocations;
    memset(data,!strcmp(path,authored)?1:!strcmp(path,ink)?2:!strcmp(path,kaiti)?4:3,bytes);
    *size=(fail_mode==2&&!strcmp(path,authored))?0:bytes;return data;
}
struct ImFont{};struct ImFontConfig{};using ImWchar=unsigned short;
static bool recovery_enabled=true;static unsigned recovery_violations;
struct Atlas {
    ImFont *AddFontFromMemoryTTF(void *data,int bytes,float,const ImFontConfig *,const ImWchar *){
        if(bytes!=128||retained)abort();++adds;
        if(recovery_enabled)++recovery_violations;
        // Owning AddFont rollback consumes input even when returning NULL.
        unsigned char id=static_cast<unsigned char *>(data)[0];
        if((fail_mode==3&&id==1)||(fail_mode==7&&id==4)){jce_free(data);return nullptr;}
        retained=data;static ImFont font;return &font;
    }
};
struct ImGuiIO{Atlas *Fonts;bool &ConfigErrorRecoveryEnableAssert;};namespace ImGui{static ImGuiIO &GetIO(){static Atlas atlas;static ImGuiIO io{&atlas,recovery_enabled};return io;}}
static void reset_selection(int mode,bool explicit_exists=true){
    if(retained||allocations!=frees)abort();style_warnings=style_infos=reads=adds=allocations=frees=0;style_message[0]=style_info[0]=0;
    fail_mode=mode;files[0]=ink;files[1]="D:/OS/Fonts/segoeui.ttf";files[2]=authored;file_count=explicit_exists?3:2;
    platform="Windows";warnings=0;memset(read_paths,0,sizeof read_paths);
    recovery_enabled=true;recovery_violations=0;
}
static bool close_atlas_port(){if(!retained)abort();jce_free(retained);retained=nullptr;return recovery_enabled&&!recovery_violations;}
'''
EDITOR_OVERRIDE_CONTROL = r'''
int main(void){
    ImFontConfig cfg;ImWchar ranges[]={32,126,0};
    reset_selection(0);
    REQUIRE(load_font_with_fallback("Latin",authored,"@ui-latin",20,&cfg,ranges));
    REQUIRE(reads==1&&adds==1&&allocations==1&&!frees&&!style_warnings&&!warnings);
    REQUIRE(!strcmp(read_paths[0],authored)&&static_cast<unsigned char *>(retained)[0]==1);
    REQUIRE(style_infos==1&&strstr(style_info,"phase=atlas_admitted role=Latin")&&strstr(style_info,authored)&&strstr(style_info,"source=user override"));REQUIRE(close_atlas_port());

    reset_selection(0,false);
    REQUIRE(load_font_with_fallback("Latin",authored,"@ui-latin",20,&cfg,ranges));
    REQUIRE(reads==1&&adds==1&&allocations==1&&style_warnings==1&&!warnings);
    REQUIRE(strstr(style_message,"override_missing_or_nonfile")&&strstr(style_message,authored));
    REQUIRE(!strcmp(read_paths[0],ink)&&static_cast<unsigned char *>(retained)[0]==2);
    REQUIRE(style_infos==1&&strstr(style_info,ink)&&strstr(style_info,"source=system font"));REQUIRE(close_atlas_port());

    for(int mode=1;mode<=3;++mode){
        reset_selection(mode);
        REQUIRE(load_font_with_fallback("Latin",authored,"@ui-latin",20,&cfg,ranges));
        REQUIRE(reads==2&&style_warnings==1&&!warnings);
        REQUIRE(!strcmp(read_paths[0],authored)&&!strcmp(read_paths[1],ink));
        REQUIRE(static_cast<unsigned char *>(retained)[0]==2);
        REQUIRE(style_infos==1&&strstr(style_info,"role=Latin")&&strstr(style_info,ink)&&strstr(style_info,"source=system font"));
        REQUIRE(strstr(style_message,mode==3?"atlas_admission_rejected":"source_read_or_size_rejected"));
        REQUIRE(strstr(style_message,"source=user override")&&strstr(style_message,authored));
        REQUIRE(allocations==(mode==1?1u:2u)&&frees==(mode==1?0u:1u)&&adds==(mode==3?2u:1u));
        REQUIRE(close_atlas_port());REQUIRE(allocations==frees);
    }

    reset_selection(5);
    REQUIRE(load_font_with_fallback("Latin",authored,"@ui-latin",20,&cfg,ranges));
    REQUIRE(reads==2&&adds==1&&allocations==2&&frees==1&&style_warnings==1);
    REQUIRE(strstr(style_message,"source_read_or_size_rejected")&&strstr(style_message,authored));
    REQUIRE(!strcmp(read_paths[1],ink));REQUIRE(close_atlas_port());

    reset_selection(3);
    for(unsigned reload=0;reload<2;++reload){
        unsigned before=style_warnings;
        REQUIRE(load_font_with_fallback("Latin",authored,"@ui-latin",20,&cfg,ranges));
        REQUIRE(style_warnings==before+1);REQUIRE(close_atlas_port());REQUIRE(allocations==frees);
    }
    REQUIRE(reads==4&&adds==4&&style_warnings==2&&style_infos==2);

    reset_selection(4);
    REQUIRE(!load_font_with_fallback("Latin",ink,"@ui-latin",20,&cfg,ranges,true));
    REQUIRE(reads==1&&!adds&&!allocations&&!retained&&style_warnings==1&&!style_infos);
    REQUIRE(strstr(style_message,"source_read_or_size_rejected"));

    reset_selection(1);files[3]=cjk_face;file_count=4;
    REQUIRE(load_font_with_fallback("CJK",authored,"@ui-cjk",20,&cfg,ranges));
    REQUIRE(reads==2&&style_warnings==1&&!strcmp(read_paths[1],cjk_face));
    REQUIRE(static_cast<unsigned char *>(retained)[0]==3);
    REQUIRE(style_infos==1&&strstr(style_info,"role=CJK")&&strstr(style_info,cjk_face)&&strstr(style_info,"source=system font"));REQUIRE(close_atlas_port());

    const char *locales[]={nullptr,"en","ja","ko","zh_cn"};
    for(const char *locale:locales){
        reset_selection(0);files[3]=cjk_face;files[4]=kaiti;file_count=5;
        REQUIRE(load_editor_cjk_font(locale,nullptr,20,&cfg,ranges));
        REQUIRE(reads==1&&!strcmp(read_paths[0],locale&&!strcmp(locale,"zh_cn")?kaiti:cjk_face));
        REQUIRE(!style_warnings&&close_atlas_port());
    }

    reset_selection(0);files[3]=cjk_face;files[4]=kaiti;file_count=5;
    REQUIRE(load_editor_cjk_font("zh_cn",authored,20,&cfg,ranges));
    REQUIRE(reads==1&&!strcmp(read_paths[0],authored)&&close_atlas_port());

    reset_selection(1);files[3]=cjk_face;files[4]=kaiti;file_count=5;
    REQUIRE(load_editor_cjk_font("zh_cn",authored,20,&cfg,ranges));
    REQUIRE(reads==2&&!strcmp(read_paths[1],kaiti)&&close_atlas_port());

    reset_selection(0);files[3]=cjk_face;file_count=4;
    REQUIRE(load_editor_cjk_font("zh_cn",nullptr,20,&cfg,ranges));
    REQUIRE(reads==1&&!strcmp(read_paths[0],cjk_face)&&!style_warnings&&close_atlas_port());

    for(int mode=6;mode<=7;++mode){
        reset_selection(mode);files[3]=cjk_face;files[4]=kaiti;file_count=5;
        REQUIRE(load_editor_cjk_font("zh_cn",kaiti,20,&cfg,ranges));
        REQUIRE(reads==2&&!strcmp(read_paths[0],kaiti)&&!strcmp(read_paths[1],cjk_face));
        REQUIRE(style_warnings==1&&close_atlas_port()&&allocations==frees);
    }

    reset_selection(0);file_count=0;
    REQUIRE(!load_font_with_fallback("Latin",nullptr,"@ui-latin",20,&cfg,ranges));
    REQUIRE(!reads&&!adds&&!allocations&&!retained&&style_warnings==1&&!style_infos&&strstr(style_message,"host_sources_exhausted"));
    return 0;
}
'''


HOST_PORTS = r'''
#include <jce/os/platform/jce_host_font.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
/* Counted portable ports exercise Windows/Linux routing, not Apple OS calls. */
#ifdef __APPLE__
#undef __APPLE__
#endif
#define REQUIRE(value) do{if(!(value)){fprintf(stderr,"host font failed %d: %s\n",__LINE__,#value);return 3;}}while(0)
#ifndef _WIN32
#define _WIN32 1
#endif
typedef unsigned long DWORD;
#define CP_UTF8 65001
static DWORD GetWindowsDirectoryW(wchar_t *out,unsigned n){if(n<6)return 0;wcscpy(out,L"D:/OS");return 5;}
static int WideCharToMultiByte(unsigned cp,unsigned flags,const wchar_t *wide,int n,char *out,int cap,const void *a,const void *b){
    (void)cp;(void)flags;(void)n;(void)a;(void)b;size_t size=wcslen(wide)+1;
    if(size>(size_t)cap)return 0;for(size_t i=0;i<size;++i)out[i]=(char)wide[i];return (int)size;
}
static const char *platform="Windows",*files[8],*fc_output="",*fc_regular_output,*fc_bold_output,*fc_ink_output;
static unsigned file_count,exists_calls,spawns,destroys,kills,warnings;
static char warning[1024],arguments[256];
static void controlled_warning(const char *tag,const char *fmt,...){
    if(strcmp(tag,"host-font"))abort();++warnings;va_list args;va_start(args,fmt);
    vsnprintf(warning,sizeof warning,fmt,args);va_end(args);
}
#define LOG_WARN controlled_warning
static bool fc_available,fc_stall;
static uint64_t ticks;
struct JceProcess{size_t offset;const char *output;};static JceProcess process;
const char *JCE_CALL jce_platform_name(void){return platform;}
int JCE_CALL jce_strcasecmp(const char *a,const char *b){
    for(;;++a,++b){unsigned x=(unsigned char)*a,y=(unsigned char)*b;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return x<y?-1:1;if(!x)return 0;}
}
bool jce_fs_host_exists_file(const char *path){++exists_calls;for(unsigned i=0;i<file_count;++i)if(!strcmp(path,files[i]))return true;return false;}
JceProcess *jce_process_spawn(const JceProcessConfig *cfg){
    ++spawns;if(!fc_available)return NULL;process.offset=0;process.output=fc_output;
    if(strcmp(cfg->executable_path,"fc-match")||!cfg->capture_stdout||cfg->capture_stderr||strlen(cfg->arguments)>=sizeof arguments)abort();
    strcpy(arguments,cfg->arguments);
    if(strstr(arguments,"Ink Free:style=Regular")&&fc_ink_output)process.output=fc_ink_output;
    if(strstr(arguments,"Segoe Print:style=Regular")&&fc_regular_output)process.output=fc_regular_output;
    if(strstr(arguments,"Segoe Print:style=Bold")&&fc_bold_output)process.output=fc_bold_output;
    return &process;
}
size_t jce_process_read_stdout(JceProcess *p,char *out,size_t cap){
    if(fc_stall)return 0;size_t remaining=strlen(p->output)-p->offset;if(remaining>cap)remaining=cap;
    memcpy(out,p->output+p->offset,remaining);p->offset+=remaining;return remaining;
}
bool jce_process_poll_exit(JceProcess *p,int *code){(void)p;if(fc_stall)return false;*code=0;return true;}
bool jce_process_force_kill(JceProcess *p){(void)p;++kills;return true;}
void jce_process_destroy(JceProcess *p){if(p!=&process)abort();++destroys;}
uint64_t JCE_CALL jce_time_ticks_ms(void){return ticks;}
void JCE_CALL jce_thread_sleep_ms(uint32_t ms){ticks+=ms;}
'''
HOST_CONTROL = r'''
int main(void){
    char out[1024];files[0]="D:/OS/Fonts/inkfree.ttf";files[1]="D:/OS/Fonts/segoeui.ttf";file_count=2;
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out));REQUIRE(!strcmp(out,files[0])&&spawns==0&&!warnings);
    REQUIRE(jce_host_find_font("InkFree",out,sizeof out));REQUIRE(!strcmp(out,files[0]));
    REQUIRE(jce_host_find_font("Ink Free",out,sizeof out)&&!strcmp(out,files[0]));
    REQUIRE(jce_host_find_font("Segoe UI",out,sizeof out));REQUIRE(!strcmp(out,files[1]));
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,3)&&!out[0]&&!warnings);
    unsigned before=exists_calls;REQUIRE(!jce_host_find_font("../font",out,sizeof out)&&!out[0]&&exists_calls==before);
    REQUIRE(!jce_host_find_font("font\" --evil",out,sizeof out)&&!out[0]);
    REQUIRE(!jce_host_default_font((JceHostFontRole)99,out,sizeof out));
    files[2]="D:/OS/Fonts/segoepr.ttf";files[3]="D:/OS/Fonts/segoeprb.ttf";files[4]="D:/OS/Fonts/msyh.ttf";file_count=5;
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0]));
    const char *regular[]={"Segoe Print","segoeprint","segoepr","Segoe Print:style=Regular"};
    const char *bold[]={"Segoe Print Bold","SegoePrint-Bold","segoeprb","Segoe Print:style=Bold"};
    for(unsigned i=0;i<4;++i){
        REQUIRE(jce_host_find_font(regular[i],out,sizeof out)&&!strcmp(out,files[2]));
        REQUIRE(jce_host_find_font(bold[i],out,sizeof out)&&!strcmp(out,files[3]));
    }
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_CJK,out,sizeof out)&&!strcmp(out,files[4])&&!warnings);
    files[4]="D:/OS/Fonts/simsun.ttc";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_CJK,out,sizeof out)&&!strcmp(out,files[4])&&!warnings);
    files[0]="D:/OS/Fonts/inkfree.otf";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[1])&&!warnings);
    files[2]="D:/OS/Fonts/segoepr.otf";file_count=3;
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[1])&&!warnings);
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]&&warnings==1);
    REQUIRE(strstr(warning,"requested=Segoe Print")&&strstr(warning,"Regular")&&strstr(warning,"unavailable"));
    platform="WASM";before=exists_calls;REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!out[0]&&exists_calls==before);
    platform="Linux";files[0]="/resolved/system.ttf";file_count=1;fc_available=true;fc_output="/resolved/system.ttf\nen|de";
    before=warnings;REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0])&&warnings==before);
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_CJK,out,sizeof out)&&!out[0]);
    fc_output="/resolved/system.ttf\nzh-cn|ja|ko";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_CJK,out,sizeof out)&&!strcmp(out,files[0]));
    REQUIRE(jce_host_find_font(":lang=ko",out,sizeof out));
    files[1]="/installed/SegoePrint.ttf";files[2]="/installed/SegoePrint-Bold.ttf";file_count=3;
    fc_regular_output="/installed/SegoePrint.ttf\nen|de\nSegoe Print\nRegular";
    fc_bold_output="/installed/SegoePrint-Bold.ttf\nen|de\nSegoe Print\nBold";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0])&&warnings==before);
    files[3]="/installed/InkFree.ttf";file_count=4;
    fc_ink_output="/installed/InkFree.ttf\nen|de\nInk Free\nRegular";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[3])&&warnings==before);
    REQUIRE(jce_host_find_font("Ink Free",out,sizeof out)&&!strcmp(out,files[3]));
    fc_ink_output="/resolved/system.ttf\nen|de\nDejaVu Sans\nRegular";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0])&&warnings==before);
    REQUIRE(!jce_host_find_font("InkFree",out,sizeof out)&&!out[0]&&warnings==before+1);before=warnings;
    fc_ink_output=NULL;
    REQUIRE(jce_host_find_font("Segoe Print",out,sizeof out)&&!strcmp(out,files[1])&&strstr(arguments,"style=Regular"));
    REQUIRE(jce_host_find_font("Segoe Print Bold",out,sizeof out)&&!strcmp(out,files[2])&&strstr(arguments,"style=Bold"));
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_CJK,out,sizeof out)&&!strcmp(out,files[0]));
    fc_regular_output="/resolved/system.ttf\nen|de\nDejaVu Sans\nRegular";
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]&&warnings==before+1);
    REQUIRE(strstr(warning,"fontconfig-substitution")&&strstr(warning,"resolved=DejaVu Sans"));
    before=warnings;REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0])&&warnings==before);
    fc_regular_output="/installed/SegoePrint-Bold.ttf\nen|de\nSegoe Print\nBold";
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]&&warnings==before+1);
    REQUIRE(strstr(warning,"resolved_style=Bold"));before=warnings;
    fc_regular_output="/installed/SegoePrint.ttf\nen|de\nSegoe Print Imposter\nRegular";
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]&&warnings==before+1);before=warnings;
    fc_regular_output="/installed/SegoePrint.ttf\nen|de\nOther alias, Segoe Print\nNormal, Regular";
    REQUIRE(jce_host_find_font("Segoe Print",out,sizeof out)&&!strcmp(out,files[1])&&warnings==before);
    fc_bold_output="/installed/SegoePrint.ttf\nen|de\nSegoe Print\nRegular";
    REQUIRE(!jce_host_find_font("Segoe Print Bold",out,sizeof out)&&!out[0]&&warnings==before+1);before=warnings;
    fc_regular_output="/installed/SegoePrint.ttf\nen|de\nSegoe Print";
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]&&warnings==before+1);before=warnings;
    char oversized[5000];memset(oversized,'x',sizeof oversized-1);oversized[sizeof oversized-1]=0;fc_regular_output=oversized;
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]&&warnings==before+1&&strstr(warning,"fontconfig-output"));
    REQUIRE(spawns==destroys);unsigned killed=kills,closed=destroys;fc_stall=true;ticks=0;
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_CJK,out,sizeof out)&&!out[0]);
    REQUIRE(kills==killed+1&&destroys==closed+1&&ticks>=1000&&ticks<=1002);
    killed=kills;closed=destroys;before=warnings;ticks=0;
    REQUIRE(!jce_host_find_font("Segoe Print",out,sizeof out)&&!out[0]);
    REQUIRE(kills==killed+1&&destroys==closed+1&&ticks>=1000&&ticks<=1002&&warnings==before+1&&strstr(warning,"fontconfig-timeout"));
    REQUIRE(spawns==destroys);
    return 0;
}
'''
SYMBOL_CONTROL = r'''
int main(void){
    char out[1024];files[0]="D:/OS/Fonts/inkfree.ttf";files[1]="D:/OS/Fonts/seguisym.ttf";file_count=2;
    REQUIRE(JCE_HOST_FONT_UI_LATIN==0&&JCE_HOST_FONT_UI_CJK==1&&JCE_HOST_FONT_UI_SYMBOLS==2);
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0]));
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!strcmp(out,files[1])&&!spawns&&!warnings);
    const char *aliases[]={"Segoe UI Symbol","SegoeUISymbol","seguisym"};
    for(unsigned i=0;i<3;++i)REQUIRE(jce_host_find_font(aliases[i],out,sizeof out)&&!strcmp(out,files[1]));
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,3)&&!out[0]&&!warnings);
    files[1]="D:/OS/Fonts/seguisym.otf";
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!out[0]&&!warnings);
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,out,sizeof out)&&!strcmp(out,files[0]));
    REQUIRE(!jce_host_find_font("Segoe UI Symbol",out,sizeof out)&&!out[0]&&warnings==1);
    platform="WASM";unsigned before=exists_calls;
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!out[0]&&exists_calls==before);
    platform="Linux";files[0]="/installed/symbols.ttf";file_count=1;fc_available=true;
    fc_output="/installed/symbols.ttf\nen|de\nSegoe Print\nRegular";
    before=spawns;unsigned warned=warnings;
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!out[0]&&spawns==before+3&&warnings==warned);
    fc_output="/installed/symbols.ttf\nen|de\nNoto Sans Symbols 2\nRegular";
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!strcmp(out,files[0])&&strstr(arguments,"Noto Sans Symbols 2"));
    fc_output="/installed/symbols.ttf\nen|de\nNoto Sans Symbols\nRegular";
    before=spawns;REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!strcmp(out,files[0])&&spawns==before+2);
    fc_output="/installed/symbols.ttf\nen|de\nNoto Sans Symbols 2\nBold";
    REQUIRE(!jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,out,sizeof out)&&!out[0]&&warnings==warned);
    fc_output="/installed/symbols.ttf\nen|de\nSegoe UI Symbol\nRegular";
    REQUIRE(jce_host_find_font("Segoe UI Symbol",out,sizeof out)&&!strcmp(out,files[0]));
    REQUIRE(spawns==destroys);return 0;
}
'''
STOCK_PORTS = r'''
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <climits>
#define LOG_TAG "controlled-stock-font"
#undef LOG_WARN
#define LOG_WARN(...) ((void)0)
static unsigned stock_ready_logs;
void JCE_CALL jce_log_write(JceLogLevel level, const char *tag, const char *, int,
                            const char *format, ...){
    if(level!=JCE_LOG_LEVEL_INFO||strcmp(tag,LOG_TAG)||!strstr(format,"stock_fallback_ready"))abort();
    ++stock_ready_logs;
}
#define REQUIRE(value) do{if(!(value)){fprintf(stderr,"stock font failed %d: %s\n",__LINE__,#value);return 3;}}while(0)
static unsigned allocations,frees;static bool fail_alloc,fail_decode,fail_verify,fail_consumer;static void *retained;
static void *owned_alloc(size_t size){if(fail_alloc)return nullptr;++allocations;return malloc(size);}
static void owned_free(void *p){if(!p)abort();++frees;free(p);}
#define ED_MALLOC owned_alloc
#define ED_FREE owned_free
struct ImFont{};struct ImFontConfig{};using ImWchar=unsigned short;
static bool recovery_enabled=true;static unsigned recovery_violations;
struct Atlas{ImFont *AddFontFromMemoryTTF(void *data,int size,float,const ImFontConfig *,const ImWchar *){if(size!=12676)abort();if(recovery_enabled)++recovery_violations;if(fail_consumer){owned_free(data);return nullptr;}retained=data;static ImFont font;return &font;}};
struct ImGuiIO{Atlas *Fonts;bool &ConfigErrorRecoveryEnableAssert;};namespace ImGui{static ImGuiIO &GetIO(void){static Atlas atlas;static ImGuiIO io={&atlas,recovery_enabled};return io;}}
static JcePakAsset asset;
static const char *expected_path = "fonts/Aileron-Regular.ttf";
const JcePakAsset *jce_pak_find(const JcePakArchive *,const char *path){if(strcmp(path,expected_path))abort();return &asset;}
size_t jce_pak_decompress_ex(const JcePakArchive *,const JcePakAsset *,void *buf,size_t size){memset(buf,0,size);return fail_decode?0:size;}
int jce_pak_verify(const JcePakAsset *,const void *,size_t){return fail_verify?0:1;}
'''
STOCK_CONTROL = r'''
int main(void){
    const JcePakArchive *pak=(const JcePakArchive *)&asset;asset.original_size=12676;ImFontConfig cfg;ImWchar ranges[]={32,126,0};
    auto load = [&](const JcePakArchive *source){return load_stock_font(source,expected_path,20,&cfg,ranges);};
    REQUIRE(!load(nullptr)&&!allocations);
    asset.original_size=0;REQUIRE(!load(pak)&&!allocations);
    asset.original_size=100;REQUIRE(!load(pak)&&!allocations);
    asset.original_size=12676;
    fail_alloc=true;REQUIRE(!load(pak)&&!allocations);fail_alloc=false;
    fail_decode=true;REQUIRE(!load(pak)&&allocations==1&&frees==1);fail_decode=false;
    fail_verify=true;REQUIRE(!load(pak)&&allocations==2&&frees==2);fail_verify=false;
    fail_consumer=true;REQUIRE(!load(pak)&&allocations==3&&frees==3&&!retained);fail_consumer=false;
    REQUIRE(recovery_enabled&&!recovery_violations&&!stock_ready_logs);
    REQUIRE(load(pak)&&allocations==4&&frees==3&&retained&&stock_ready_logs==1);owned_free(retained);retained=nullptr;REQUIRE(frees==4);
    expected_path="fonts/JCE-CJK-Fallback.ttf";
    REQUIRE(load(pak)&&allocations==5&&frees==4&&retained&&stock_ready_logs==2);owned_free(retained);retained=nullptr;REQUIRE(frees==5);
    REQUIRE(recovery_enabled&&!recovery_violations);
    return 0;
}
'''


NATIVE_WINDOWS_PORTS = r'''
#include <stdarg.h>
#define REQUIRE(value) do{if(!(value)){fprintf(stderr,"native host font failed %d: %s\n",__LINE__,#value);return 3;}}while(0)
static unsigned native_spawns,native_warnings;
const char *JCE_CALL jce_platform_name(void){return "Windows";}
int JCE_CALL jce_strcasecmp(const char *a,const char *b){return _stricmp(a,b);}
bool jce_fs_host_exists_file(const char *path){
    wchar_t wide[4096];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096))return false;
    DWORD flags=GetFileAttributesW(wide);return flags!=INVALID_FILE_ATTRIBUTES&&!(flags&FILE_ATTRIBUTE_DIRECTORY);
}
JceProcess *jce_process_spawn(const JceProcessConfig *cfg){(void)cfg;++native_spawns;return NULL;}
size_t jce_process_read_stdout(JceProcess *p,char *out,size_t cap){(void)p;(void)out;(void)cap;return 0;}
bool jce_process_poll_exit(JceProcess *p,int *code){(void)p;(void)code;return false;}
bool jce_process_force_kill(JceProcess *p){(void)p;return false;}
void jce_process_destroy(JceProcess *p){(void)p;abort();}
uint64_t JCE_CALL jce_time_ticks_ms(void){return GetTickCount64();}
void JCE_CALL jce_thread_sleep_ms(uint32_t ms){Sleep(ms);}
void JCE_CALL jce_log_write(JceLogLevel level,const char *tag,const char *file,int line,const char *fmt,...){
    (void)tag;(void)file;(void)line;if(level==JCE_LOG_LEVEL_WARN)++native_warnings;
    va_list args;va_start(args,fmt);vfprintf(stderr,fmt,args);fputc('\n',stderr);va_end(args);
}
int main(void){
    char regular[4096],bold[4096],selected[4096],expected[4096];
    REQUIRE(jce_host_find_font("Segoe Print",regular,sizeof regular));
    REQUIRE(jce_host_find_font("Segoe Print Bold",bold,sizeof bold));
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,selected,sizeof selected));
    REQUIRE(host_font_windows_directory(expected,sizeof expected));
    strcat(expected,"/Fonts/inkfree.ttf");
    if(!jce_fs_host_exists_file(expected))REQUIRE(jce_host_find_font("Segoe UI",expected,sizeof expected));
    REQUIRE(!strcmp(expected,selected)&&strcmp(regular,bold)&&!native_spawns&&!native_warnings);
    printf("REGULAR=%s\nBOLD=%s\nDEFAULT=%s\n",regular,bold,selected);
    return 0;
}
'''


def native_windows_probe():
    source = (ROOT / "engine/src/os/platform/jce_host_font.c").read_text(encoding="utf-8")
    return source + "\n" + NATIVE_WINDOWS_PORTS


def native_windows_symbol_probe():
    source = (ROOT / "engine/src/os/platform/jce_host_font.c").read_text(encoding="utf-8")
    ports = NATIVE_WINDOWS_PORTS.split("int main(void){", 1)[0]
    control = r'''
int main(void){
    char selected[4096],explicit_path[4096];
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS,selected,sizeof selected));
    REQUIRE(jce_host_find_font("Segoe UI Symbol",explicit_path,sizeof explicit_path));
    REQUIRE(!strcmp(selected,explicit_path)&&!native_spawns&&!native_warnings);
    printf("SYMBOLS=%s\n",selected);return 0;
}
'''
    return source + "\n" + ports + control


class StockProbeCleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = ROOT / "build/tests/host-font-selection-tests"
        cls.base.mkdir(parents=True, exist_ok=True)

    def holder(self, side_effect=None):
        holder = mock.Mock()
        holder.name = str(self.base / "owned-cleanup-control")
        holder.cleanup.side_effect = side_effect
        return holder

    @staticmethod
    def refusal(code):
        error = PermissionError(13, "controlled deletion refusal")
        error.winerror = code
        return error

    def test_success_does_not_wait_or_repeat_cleanup(self):
        holder, waits = self.holder(), []
        cleanup_stock_directory(holder, self.base, windows=True, pause=waits.append)
        holder.cleanup.assert_called_once_with()
        self.assertEqual(waits, [])

    def test_windows_access_and_sharing_refusals_retry_then_succeed(self):
        for code in (5, 32, 33):
            with self.subTest(winerror=code):
                holder, waits = self.holder([self.refusal(code), None]), []
                cleanup_stock_directory(holder, self.base, windows=True, pause=waits.append)
                self.assertEqual(holder.cleanup.call_count, 2)
                self.assertEqual(waits, [0.05])

    def test_exhaustion_is_bounded_and_rethrows_the_actual_error(self):
        error, waits = self.refusal(5), []
        holder = self.holder(error)
        with self.assertRaises(PermissionError) as caught:
            cleanup_stock_directory(holder, self.base, windows=True, pause=waits.append)
        self.assertIs(caught.exception, error)
        self.assertEqual(holder.cleanup.call_count, 5)
        self.assertEqual(waits, [0.05, 0.1, 0.2, 0.4])

    def test_other_platform_and_unrelated_errors_are_not_retried(self):
        for windows, error in ((False, self.refusal(5)),
                (True, self.refusal(123)), (True, OSError("unrelated I/O error"))):
            with self.subTest(windows=windows, error=error):
                holder, waits = self.holder(error), []
                with self.assertRaises(type(error)) as caught:
                    cleanup_stock_directory(holder, self.base, windows=windows, pause=waits.append)
                self.assertIs(caught.exception, error)
                holder.cleanup.assert_called_once_with()
                self.assertEqual(waits, [])

    def test_fixture_root_and_escaped_target_are_refused_before_cleanup(self):
        for path in (self.base, self.base.parent / "unowned-cleanup-control"):
            with self.subTest(path=path):
                holder, waits = self.holder(), []
                holder.name = str(path)
                with self.assertRaisesRegex(ValueError, "escaped"):
                    cleanup_stock_directory(holder, self.base, windows=True, pause=waits.append)
                holder.cleanup.assert_not_called()
                self.assertEqual(waits, [])

    def test_retry_revalidates_the_actual_directory_before_cleanup(self):
        holder = self.holder([self.refusal(5), None])
        waits = []
        def change_target(delay):
            waits.append(delay)
            holder.name = str(self.base.parent / "unowned-cleanup-control")
        with self.assertRaisesRegex(ValueError, "escaped"):
            cleanup_stock_directory(holder, self.base, windows=True, pause=change_target)
        holder.cleanup.assert_called_once_with()
        self.assertEqual(waits, [0.05])

    def test_body_assertion_remains_primary_if_cleanup_also_fails(self):
        holder = StockProbeDirectory(self.base)
        body, cleanup = AssertionError("ordinary assertion"), self.refusal(5)
        try:
            with mock.patch(__name__ + ".cleanup_stock_directory", side_effect=cleanup):
                with self.assertRaises(AssertionError) as caught:
                    with holder:
                        raise body
            self.assertIs(caught.exception, body)
            self.assertIs(caught.exception.__cause__, cleanup)
        finally:
            cleanup_stock_directory(holder, self.base, windows=os.name == "nt")

    def test_cleanup_interrupt_is_not_replaced_by_a_prior_assertion(self):
        holder = StockProbeDirectory(self.base)
        interrupt = KeyboardInterrupt("controlled cleanup interrupt")
        try:
            with mock.patch(__name__ + ".cleanup_stock_directory", side_effect=interrupt):
                with self.assertRaises(KeyboardInterrupt) as caught:
                    with holder:
                        raise AssertionError("ordinary assertion")
            self.assertIs(caught.exception, interrupt)
        finally:
            cleanup_stock_directory(holder, self.base, windows=os.name == "nt")

    def test_actual_owned_directory_is_removed_after_body_failure(self):
        holder = StockProbeDirectory(self.base)
        with self.assertRaisesRegex(AssertionError, "ordinary assertion"):
            with holder as temporary:
                (Path(temporary) / "owned.txt").write_text("probe", encoding="ascii")
                raise AssertionError("ordinary assertion")
        self.assertFalse(Path(holder.name).exists())


class HostFontSelectionTests(unittest.TestCase):
    def test_actual_symbols_role_alias_missing_and_exact_fontconfig_family_policy(self):
        C_RUN.execute(self, host_probe(symbols=True))

    def test_negative_old_latin_cjk_only_roles_are_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("skip_symbols", symbols=True))

    def test_negative_symbols_fontconfig_substitution_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("fc_family", symbols=True))

    def test_actual_windows_symbols_path_family_and_authored_codepoint(self):
        if os.name != "nt":
            self.skipTest("Actual Windows Symbols metadata requires Windows")
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest("fontTools is required for actual Symbols metadata")
        expected = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts/seguisym.ttf"
        if not expected.is_file():
            self.skipTest("Segoe UI Symbol is not installed; counted absence controls still apply")
        output = execute_stock(self, native_windows_symbol_probe())
        paths = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
        path = Path(paths["SYMBOLS"])
        self.assertEqual(path.resolve(), expected.resolve())
        with TTFont(path, lazy=True) as font:
            self.assertEqual(font["name"].getDebugName(1), "Segoe UI Symbol")
            glyph = font.getBestCmap().get(0x23ED)
            self.assertIsNotNone(glyph)
            self.assertGreater(font.getGlyphID(glyph), 0)

    def test_actual_platform_default_family_language_and_timeout_policy(self):
        C_RUN.execute(self, host_probe())

    def test_negative_old_print_default_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("old_print"))

    def test_negative_false_cjk_coverage_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("cjk"))

    def test_negative_old_sans_only_default_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("skip_ink"))

    def test_negative_print_bold_resolving_regular_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("wrong_bold"))

    def test_negative_fontconfig_substitution_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("fc_family"))

    def test_negative_fontconfig_wrong_style_is_detected(self):
        C_RUN.execute(NegativeRuntime(self), host_probe("fc_style"))

    def test_actual_windows_installed_print_paths_and_face_metadata(self):
        if os.name != "nt":
            self.skipTest("Actual installed Windows Print probe requires Windows")
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest("fontTools is required to inspect actual Windows face metadata")
        directory = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts"
        if not all((directory / name).is_file() for name in ("segoepr.ttf", "segoeprb.ttf")):
            self.skipTest("Segoe Print Regular/Bold are not both installed; counted fallback controls remain applicable")
        output = execute_stock(self, native_windows_probe())
        paths = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
        self.assertEqual(Path(paths["DEFAULT"]).resolve(), (directory / ("inkfree.ttf" if (directory / "inkfree.ttf").is_file() else "segoeui.ttf")).resolve())
        for role, filename, style, postscript, weight in (
            ("REGULAR", "segoepr.ttf", "Regular", "SegoePrint", 400),
            ("BOLD", "segoeprb.ttf", "Bold", "SegoePrint-Bold", 700),
        ):
            path = Path(paths[role])
            self.assertEqual(path.name.lower(), filename)
            self.assertEqual(path.resolve(), (directory / filename).resolve())
            self.assertEqual(path.suffix.lower(), ".ttf")
            with TTFont(path, lazy=True) as font:
                names = font["name"]
                self.assertEqual(names.getDebugName(1), "Segoe Print")
                self.assertEqual(names.getDebugName(2), style)
                self.assertEqual(names.getDebugName(6), postscript)
                self.assertEqual(font["OS/2"].usWeightClass, weight)
                self.assertEqual(bool(font["OS/2"].fsSelection & 0x20), role == "BOLD")

    def test_actual_windows_ink_free_default_path_and_metadata(self):
        if os.name != "nt":
            self.skipTest("Actual Ink Free metadata requires Windows")
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest("fontTools is required for actual Ink Free metadata")
        expected = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts/inkfree.ttf"
        if not expected.is_file():
            self.skipTest("Ink Free is not installed; counted fallback controls still apply")
        source = (ROOT / "engine/src/os/platform/jce_host_font.c").read_text(encoding="utf-8")
        ports = NATIVE_WINDOWS_PORTS.split("int main(void){", 1)[0]
        control = r'''
int main(void){
    char selected[4096],explicit_path[4096];
    REQUIRE(jce_host_default_font(JCE_HOST_FONT_UI_LATIN,selected,sizeof selected));
    REQUIRE(jce_host_find_font("Ink Free",explicit_path,sizeof explicit_path));
    REQUIRE(!strcmp(selected,explicit_path)&&!native_spawns&&!native_warnings);
    printf("INKFREE=%s\n",selected);return 0;
}
'''
        output = execute_stock(self, source + "\n" + ports + control)
        paths = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
        path = Path(paths["INKFREE"])
        self.assertEqual(path.resolve(), expected.resolve())
        with TTFont(path, lazy=True) as font:
            self.assertEqual(font["name"].getDebugName(1), "Ink Free")
            self.assertEqual(font["name"].getDebugName(2), "Regular")
            self.assertEqual(font["OS/2"].usWeightClass, 400)

    def test_actual_owning_transfer_retains_success_and_does_not_repeat_rollback_free(self):
        execute_stock(self, owning_transfer_probe())

    def test_negative_old_transfer_null_cleanup_is_ordinary_failure(self):
        execute_stock(self, owning_transfer_probe(True), negative=True,
            negative_exit=3, negative_marker="owning font transfer failed ")

    def test_actual_tiny_source_and_scoped_user_error_owner_restore(self):
        execute_stock(self, recovery_probe())

    def test_negative_missing_scoped_user_error_guard_is_ordinary_failure(self):
        execute_stock(self, recovery_probe(True), negative=True,
            negative_exit=3, negative_marker="recoverable font owner failed ")

    def test_actual_editor_override_diagnostics_and_shared_role_retry(self):
        execute_stock(self, editor_override_probe())

    def test_negative_old_override_failure_bypasses_shared_role(self):
        execute_stock(self, editor_override_probe(True), negative=True,
            negative_exit=3, negative_marker="host font failed ")

    def test_actual_editor_stock_allocation_decode_admission_and_transfer(self):
        execute_stock(self, stock_probe())

    def test_negative_stock_decode_leak_is_detected(self):
        execute_stock(self, stock_probe(True), negative=True,
            negative_exit=3, negative_marker="stock font failed ")

    def test_stock_binary_and_license_provenance_are_exact_and_latin_only(self):
        root=ROOT / "engine/ui/fonts"
        data=(root / "Aileron-Regular.ttf").read_bytes()
        provenance=json.loads((root / "Aileron-provenance.json").read_text(encoding="utf-8"))
        self.assertEqual(len(data),12676)
        self.assertEqual(data[:4],b"\x00\x01\x00\x00")
        self.assertEqual(hashlib.sha256(data).hexdigest(),provenance["sha256"])
        self.assertEqual(provenance["glyph_count"],113)
        self.assertFalse(provenance["cjk_coverage"])
        self.assertIn("No Rights Reserved",(root / "Aileron-LICENSE.txt").read_text(encoding="utf-8"))

    def test_local_custom_font_is_ignored_without_requiring_its_bytes(self):
        # The counted editor override control above supplies its own font data.
        # This policy check applies even when the optional authored font is absent.
        relative = "editor/resources/assets/fonts/JCE.ttf"
        result = subprocess.run(["git", "check-ignore", "-v", "--", relative],
            cwd=ROOT, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        rule, _, matched = result.stdout.rstrip("\n").partition("\t")
        self.assertEqual(matched, relative)
        self.assertRegex(rule, r"^\.gitignore:\d+:editor/resources/assets/fonts/$")

    def test_stock_cjk_font_matches_packaging_provenance_and_budget(self):
        root = ROOT / "engine/ui/fonts"
        data = (root / "JCE-CJK-Fallback.ttf").read_bytes()
        provenance = json.loads((root / "JCE-CJK-Fallback-provenance.json").read_text(encoding="utf-8"))
        self.assertEqual(provenance["asset"], "fonts/JCE-CJK-Fallback.ttf")
        self.assertEqual(provenance["license"], "OFL-1.1")
        self.assertTrue(provenance["modified"])
        self.assertEqual(len(data), provenance["bytes"])
        self.assertLessEqual(len(data), 3 * 1024 * 1024)
        self.assertEqual(data[:4], b"\x00\x01\x00\x00")
        self.assertEqual(hashlib.sha256(data).hexdigest(), provenance["sha256"])
        notice = (root / "JCE-CJK-Fallback-OFL.txt").read_text(encoding="utf-8")
        self.assertIn("SIL OPEN FONT LICENSE", notice.upper())
        self.assertIn("The Klee Project Authors", notice)

    def test_stock_cjk_cmap_covers_editor_translations_and_breadcrumb(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest("fontTools is required to inspect the stock TTF cmap")
        root = ROOT / "engine/ui/fonts"
        provenance = json.loads((root / "JCE-CJK-Fallback-provenance.json").read_text(encoding="utf-8"))
        translations = ROOT / "editor/resources/assets/i18n"
        paths = sorted(translations.glob("*.json"))
        self.assertTrue(paths)
        locales = (json.loads(path.read_text(encoding="utf-8")) for path in paths)
        required = {ord(char) for strings in locales for value in strings.values()
                    if isinstance(value, str) for char in value if ord(char) >= 128}
        with TTFont(root / "JCE-CJK-Fallback.ttf", lazy=True) as font:
            cmap = font.getBestCmap()
            self.assertEqual(len(cmap), provenance["codepoints"])
            self.assertEqual(font["maxp"].numGlyphs, provenance["glyphs"])
            self.assertEqual(font["name"].getDebugName(1), provenance["family"])
            missing = required.difference(cmap)
            self.assertFalse(missing, [f"U+{point:04X}" for point in sorted(missing)])
            self.assertEqual(cmap[0x25B8], cmap[0x25B6])
            self.assertTrue({0x25C6, 0x25C9, 0x25CB}.issubset(cmap))


if __name__ == "__main__":
    unittest.main()
