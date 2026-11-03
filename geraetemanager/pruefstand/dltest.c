#include <stdio.h>
#define DL_TEST_LINUX
#include "/home/claude/geraetemanager/src/pcidl.h"
static void fs(DWORD n){ (void)n; }
int main(int c,char**v){
    BYTE* d; DWORD n; char f[600];
    BOOL ok=DlHolen(v[1],&d,&n,f,sizeof(f),fs);
    if(ok){ FILE* o=fopen("geholt.ids","wb"); fwrite(d,1,n,o); fclose(o); printf("OK %u Byte\n",n); }
    else printf("FEHLER: %s\n",f);
    return 0;
}
