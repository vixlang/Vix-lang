#! /bin/bash
# file="main.ll"


set -e
echo "rm build and runtime bin files..."
cd src
echo "build seed comiler"
ulimit -s 65536 && ../build/vixc main.vix -ll
mv main.ll ../seed/
cd ..
cd seed
rm vixc.ll
mv main.ll vixc.ll 
rm vixc
echo "build OK!"

cd ..
rm -rf build runtime
sleep 5
echo "git commit ..."
git add .
git commit -m "$1"
git push
echo "ALL OK!"
