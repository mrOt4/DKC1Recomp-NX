#include "windows_music_pack.h"
#include "dkc1_msu1.h"
#include <miniz.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc==2);char archive[4096],error[256];snprintf(archive,sizeof archive,"%s/synthetic.zip",argv[1]);
 CHECK(Dkc1WindowsMusicTrackName("../../track-1.pcm")==1); /* safe flattened basename */
 CHECK(Dkc1WindowsMusicTrackName("track-1.pcm.exe")==0);CHECK(Dkc1WindowsMusicTrackName("track-33.pcm")==0);
 CHECK(Dkc1WindowsMusicTrackName("dkc_msu-7.ogg")==7);CHECK(Dkc1WindowsMusicTrackName("track-1.ogg.exe")==0);
 unsigned char pcm[8+441*4]={'M','S','U','1',0,0,0,0};for(size_t i=8;i<sizeof pcm;i+=2){pcm[i]=0x40;pcm[i+1]=0x20;}
 mz_zip_archive zip={0};CHECK(mz_zip_writer_init_file(&zip,archive,0));
 CHECK(mz_zip_writer_add_mem(&zip,"../../track-1.pcm",pcm,sizeof pcm,MZ_BEST_SPEED));
 CHECK(mz_zip_writer_finalize_archive(&zip));CHECK(mz_zip_writer_end(&zip));
 char *directory=Dkc1WindowsExtractMusicPack(archive,argv[1],error,sizeof error);CHECK(directory);
 CHECK(strstr(directory,"music-packs")!=NULL);
 Dkc1Msu1 *player=Dkc1Msu1Open(directory,error,sizeof error);CHECK(player);
 Dkc1Msu1ObserveMusicState(player,0,1);CHECK(Dkc1Msu1CurrentTrack(player)==1);
 int16_t samples[128]={0};Dkc1Msu1Mix(player,samples,64,2,32040);int nonzero=0;for(int i=0;i<128;i++)nonzero|=samples[i];CHECK(nonzero);
 Dkc1Msu1Reset(player);Dkc1Msu1Close(player);free(directory);
 memset(&zip,0,sizeof zip);CHECK(mz_zip_writer_init_file(&zip,archive,0));
 CHECK(mz_zip_writer_add_mem(&zip,"a/track-1.pcm",pcm,sizeof pcm,0));CHECK(mz_zip_writer_add_mem(&zip,"b/dkc_msu-1.pcm",pcm,sizeof pcm,0));
 CHECK(mz_zip_writer_finalize_archive(&zip));CHECK(mz_zip_writer_end(&zip));
 CHECK(!Dkc1WindowsExtractMusicPack(archive,argv[1],error,sizeof error));
 puts("WINDOWS_MUSIC_PASS: safe extraction, duplicate rejection, mapped PCM mixing and reset");return 0;
}
