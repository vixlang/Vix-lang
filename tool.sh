#! /bin/bash
# file="main.ll"


set -e
echo "rm build and runtime bin files..."
cd src
echo "build seed comiler"
../build/vixc main.vix -ll
mv main.ll ../seed/
cd ..
cd seed
rm vixc.ll
mv main.ll vixc.ll 
rm vixc
echo "build OK!"

cd ..
rm -rf build runtime
cd ..
ls bootstrap/
sleep 5
echo "git commit ..."
git add .
git commit -m "$1"
git push
echo "ALL OK!"
