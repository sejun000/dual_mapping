# fileserverfs: stock fileserver (128k files, fill 50, 50 threads) with an fsync after the whole-file write and after the append
# (every file write is made durable before the next op, like O_DSYNC; 2026-09-10).
#set mode quit alldone

set $dir=/mnt/f2fs/test
set $nfiles=178905

set $meandirwidth=20
set $filesize=128k
set $nthreads=50
set $iosize=1m
set $meanappendsize=16k

define fileset name=bigfileset,path=$dir,size=$filesize,entries=$nfiles,dirwidth=$meandirwidth,prealloc=80

define process name=filereader,instances=1
{
  thread name=filereaderthread,memsize=10m,instances=$nthreads
  {
    flowop createfile name=createfile1,filesetname=bigfileset,fd=1
    flowop writewholefile name=wrtfile1,srcfd=1,fd=1,iosize=$iosize
    flowop fsync name=fsyncfile1,fd=1
    flowop closefile name=closefile1,fd=1
    flowop openfile name=openfile1,filesetname=bigfileset,fd=1
    flowop appendfilerand name=appendfilerand1,iosize=$meanappendsize,fd=1
    flowop fsync name=fsyncappend1,fd=1
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile2,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile1,fd=1,iosize=$iosize
    flowop closefile name=closefile3,fd=1
    flowop deletefile name=deletefile1,filesetname=bigfileset
    flowop statfile name=statfile1,filesetname=bigfileset
  }
}

echo "File-server 75% Version 3.0 personality successfully loaded"

create files
system "sync"
system "sh -c 'echo 3 > /proc/sys/vm/drop_caches'"
echo "prefill sync and cache drop done"
psrun 600 600
