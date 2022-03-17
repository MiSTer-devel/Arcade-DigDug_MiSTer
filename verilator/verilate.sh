#export OPTIMIZE="-O3 --x-assign fast --x-initial fast --noassert"
export OPTIMIZE="--x-assign fast --x-initial fast --noassert"
export WARNINGS="-Wno-WIDTH -Wno-MULTIDRIVEN"

set -e
if grep -qEi "(Microsoft|WSL)" /proc/version &> /dev/null ; then
verilator \
-cc --compiler msvc +define+SIMULATION=1 $WARNINGS $OPTIMIZE \
--converge-limit 6000 \
--top-module emu sim.v \
-I../rtl \
-I../rtl/cpu \
dprams.v \
cpucore.v \
wsg.v
else
	echo "not running on windows"
fi
