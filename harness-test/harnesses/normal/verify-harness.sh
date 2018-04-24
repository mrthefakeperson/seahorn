#!/bin/sh
HARNESS=ll/$(basename $1)-H.ll
STDOUT=stdout/$(basename $1).stdout
STDERR=stderr/$(basename $1).stderr
DEBUG=debug/$(basename $1)-debug
DETAILS=details/$(basename $1)-details.txt
LOG=log/$(basename $1)-log.txt
LOG2=log/$(basename $1)-log2.txt

echo $1

CMD="~/seahorn/seahorn/build/run/bin/sea pf -O3 -m64 --inline --step=large \
  --track=mem --horn-global-constraints=true --enable-nondet-init \
  --strip-extern --externalize-addr-taken-functions \
  --horn-singleton-aliases=true --devirt-functions \
  --horn-ignore-calloc=false --enable-indvar --enable-loop-idiom \
  --symbolize-constant-loop-bounds --unfold-loops-for-dsa \
  --simplify-pointer-loops --horn-sea-dsa-split --dsa=sea-ci \
  --cex=$HARNESS --horn-cex-bv=true --cpu=600 --mem=4000 \
  --horn-cex-bv-memsim=true $1 --temp-dir=./temps --keep-temps --log=cex -g"
echo $CMD > $LOG
eval $CMD > $LOG2 2>&1
grep 'enter:' $LOG2 > $LOG2-tmp
mv $LOG2-tmp $LOG2
echo " >> pf"
[ -e "$HARNESS" ] || exit 0

CMD="~/seahorn/seahorn/build/run/bin/sea exe -m64 -g $1 $HARNESS -o $DEBUG \
  --temp-dir=./temps --keep-temps"
echo $CMD >> $LOG
eval $CMD > /dev/null 2>&1
if [ ! -e "$DEBUG" ]; then
  echo "could not link harness" > $DETAILS
  exit 1
fi
echo " >> exe"

CMD="/usr/bin/time -o $DETAILS -f \"execution summary for $(basename $1):\ntime, s: \n%S\n +\n%U\nmemory, kB, max: \n%M\nkB, avg\n%K\" \
  timeout 30 \
    $DEBUG > $STDOUT 2> $STDERR"
echo $CMD >> $LOG
eval $CMD
if [ $? = 124 ]; then
  echo "TIME LIMIT EXCEEDED" | tee $DETAILS --append
fi
stat --printf="file size: (code)\n%s\n" $1 >> $DETAILS
stat --printf="file size: (harness)\n%s\n" $HARNESS >> $DETAILS

if [ -s $STDERR ]; then
  echo "RUNTIME ERROR" | tee $DETAILS --append
elif [ -e $STDERR ]; then
  rm $STDERR
fi

while read LINE; do
  if [ "$LINE" = "[sea] exiting with a call to __assert_fail" ] || [ "$LINE" = "[sea] __VERIFIER_error was executed" ]; then
    echo "SUCCESSFUL HARNESS" | tee $DETAILS --append
#    rm $DEBUG
    exit 0
  fi
done < $STDOUT
echo "FAILED HARNESS" | tee $DETAILS --append
#rm $DEBUG

