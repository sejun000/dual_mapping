# varmail-hot1g: varmail-derived hot-mailbox microworkload whose rewrites sit
# in the 128KB-8MB "window band". bigfileset is created identically to
# varmail-50-perc-fsutil-600s (it is the 50% fill) but no flowop touches it;
# the active set is 512 fixed 8KB mail files (4MB) that are rewritten in place
# (writewholefile+fsync) and read back. Every rewrite of a file lands ~4MB of
# log traffic after its previous version, so an 8MiB IPU window can absorb it
# and a ZRWA-sized (128KB) one cannot.
#
# The first design churned the hot set varmail-style (delete/create/append):
# under 16 threads on a real device, skipped deletes let the fileset drift to
# full (511/512 existing at death) and createfile starved ~57s in. A fixed
# set with in-place rewrites has no fileset dynamics and a stationary 4MB
# working set.
set $dir=/mnt/f2fs/test
set $nfiles=1184222
set $meandirwidth=1000000
set $nhotfiles=2048
set $nthreads=16
set $iosize=1m

define fileset name=bigfileset,path=$dir,size=16k,entries=$nfiles,dirwidth=$meandirwidth,prealloc=80
define fileset name=hotset,path=$dir,size=512k,entries=$nhotfiles,dirwidth=128,prealloc=100

define process name=filereader,instances=1
{
  thread name=filereaderthread,memsize=10m,instances=$nthreads
  {
    flowop openfile name=openfile2,filesetname=hotset,fd=1
    flowop writewholefile name=writefile2,iosize=512k,fd=1
    flowop fsync name=fsyncfile2,fd=1
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile3,filesetname=hotset,fd=1
    flowop readwholefile name=readfile3,fd=1,iosize=$iosize
    flowop closefile name=closefile3,fd=1
  }
}

echo "Varmail hot-mailbox (1GB fixed active set, in-place rewrite) loaded"

create files
system "sync"
system "sh -c 'echo 3 > /proc/sys/vm/drop_caches'"
echo "prefill sync and cache drop done"
psrun 600 600
