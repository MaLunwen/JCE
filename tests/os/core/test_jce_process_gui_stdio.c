/* Reusable PUBLIC-SDK Windows GUI/self-child control. No private symbols,
 * SDL headers, SetStdHandle, console allocation or synthetic inheritance.
 * Default mode requires actual absent parent stdout/stderr (else SKIP 77).
 * Optional --inherited accepts the launcher-provided handles for comparison.
 * The final optional argument is an owned UTF-8 receipt path, quoted if needed. */
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
static char receipt[4096],receipt_path[2048];
static size_t receipt_size;
static void note(const char *format,...)
{
    if(receipt_size>=sizeof receipt-1)return;
    va_list args;va_start(args,format);
    int n=vsnprintf(receipt+receipt_size,sizeof receipt-receipt_size,format,args);va_end(args);
    if(n>0)receipt_size+=(size_t)n<sizeof receipt-receipt_size ? (size_t)n : sizeof receipt-receipt_size-1;
}
static int finish(int result)
{note("result=%d\n",result);return jce_fs_host_write_all(receipt_path,receipt,receipt_size) ? result : 92;}
static bool present(DWORD stream)
{DWORD flags=0;HANDLE handle=GetStdHandle(stream);
 return handle && handle!=INVALID_HANDLE_VALUE && GetHandleInformation(handle,&flags);}
static const wchar_t *arguments(void)
{
    const wchar_t *p=GetCommandLineW();bool quoted=false;
    while(*p){if(*p==L'"')quoted=!quoted;else if(!quoted && (*p==L' ' || *p==L'\t'))break;++p;}
    while(*p==L' ' || *p==L'\t')++p;return p;
}
static bool write_stream(DWORD stream,const char *text)
{DWORD count=0;return WriteFile(GetStdHandle(stream),text,(DWORD)strlen(text),&count,NULL) && count==strlen(text);}
static int child(bool wait)
{
    if(!write_stream(STD_OUTPUT_HANDLE,"stdout-payload\n") || !write_stream(STD_ERROR_HANDLE,"stderr-payload\n"))return 91;
    if(wait)jce_thread_sleep_ms(10000);
    return 23;
}
static bool drain(JceProcess *process,char *out,size_t *out_size,char *err,size_t *err_size)
{
    if(*out_size>=63 || *err_size>=63)return false;
    *out_size+=jce_process_read_stdout(process,out+*out_size,63-*out_size);
    *err_size+=jce_process_read_stderr(process,err+*err_size,63-*err_size);
    out[*out_size]=0;err[*err_size]=0;return *out_size<63 && *err_size<63;
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    (void)instance;(void)previous;(void)command;(void)show;
    const wchar_t *args=arguments();
    if(!wcscmp(args,L"--child-io"))return child(false);
    if(!wcscmp(args,L"--child-wait"))return child(true);
    bool inherit=!wcsncmp(args,L"--inherited",11) && (args[11]==0 || args[11]==L' ');
    if(inherit){args+=11;while(*args==L' ' || *args==L'\t')++args;}
    if(*args){
        size_t size=wcslen(args);if(size>=2 && args[0]==L'"' && args[size-1]==L'"'){++args;size-=2;}
        if(size>1023 || !WideCharToMultiByte(CP_UTF8,0,args,(int)size,receipt_path,sizeof receipt_path-1,NULL,NULL))return 92;
    }else{
        if(!jce_fs_host_get_current_dir(receipt_path,sizeof receipt_path))return 92;
        size_t size=strlen(receipt_path);snprintf(receipt_path+size,sizeof receipt_path-size,"/_ut_process_gui_stdio_pid%lu.txt",GetCurrentProcessId());
    }
    bool out_present=present(STD_OUTPUT_HANDLE),err_present=present(STD_ERROR_HANDLE);
    note("mode=%s parent_stdout_present=%d parent_stderr_present=%d\n",inherit ? "inherited" : "absent",out_present,err_present);
    if(!inherit && (out_present || err_present)){note("state=SKIP_parent_handles_present\n");return finish(77);}
    wchar_t wide_exe[1024];char executable[4096];
    DWORD size=GetModuleFileNameW(NULL,wide_exe,1024);
    if(!size || size>=1024 || !WideCharToMultiByte(CP_UTF8,0,wide_exe,-1,executable,sizeof executable,NULL,NULL))return finish(93);
    for(unsigned round=0;round<3;++round)for(unsigned bits=0;bits<4;++bits){
        JceProcessConfig config={executable,NULL,"--child-io",(bits&1)!=0,(bits&2)!=0};
        JceProcess *process=jce_process_spawn(&config);
        if(!process){note("state=FAIL_spawn round=%u capture=%u error=%s\n",round,bits,jce_process_get_last_spawn_error());return finish(94);}
        char out[64]={0},err[64]={0};size_t out_size=0,err_size=0;int code=-1;bool exited=false;
        uint64_t start=jce_time_ticks_ms();
        while(jce_time_ticks_ms()-start<5000){
            if(!drain(process,out,&out_size,err,&err_size))break;
            if(jce_process_poll_exit(process,&code)){exited=true;break;}jce_thread_sleep_ms(1);
        }
        if(exited)for(unsigned i=0;i<2;++i)(void)drain(process,out,&out_size,err,&err_size);
        if(!exited)(void)jce_process_force_kill(process);
        jce_process_destroy(process);
        bool match=exited && code==23 && out_size==(config.capture_stdout ? 15u : 0u) &&
            err_size==(config.capture_stderr ? 15u : 0u) &&
            (!config.capture_stdout || !strcmp(out,"stdout-payload\n")) &&
            (!config.capture_stderr || !strcmp(err,"stderr-payload\n"));
        note("round=%u capture=%u exited=%d exit=%d stdout_bytes=%llu stderr_bytes=%llu matched=%d\n",
            round,bits,exited,code,(unsigned long long)out_size,(unsigned long long)err_size,match);
        if(!match){note("state=FAIL_payload_or_exit\n");return finish(95);}
    }
    JceProcessConfig config={executable,NULL,"--child-wait",true,true};
    JceProcess *process=jce_process_spawn(&config);if(!process){note("state=FAIL_kill_spawn\n");return finish(96);}
    bool killed=jce_process_force_kill(process),exited=false;int code=-1;
    uint64_t start=jce_time_ticks_ms();
    while(jce_time_ticks_ms()-start<5000){if(jce_process_poll_exit(process,&code)){exited=true;break;}jce_thread_sleep_ms(1);}
    jce_process_destroy(process);
    note("force_kill=%d reaped=%d exit=%d\n",killed,exited,code);
    if(!killed || !exited || code==0){note("state=FAIL_kill_reap\n");return finish(97);}
    note("state=PASS_actual_public_process\n");return finish(0);
}
