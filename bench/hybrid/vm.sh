#!/usr/bin/env bash
# C1's deciding rows in document-gate's Linux VM (8 vCPU, 16 GiB, C1's shape), from a checkout:
#   vm.sh setup          Docker and C1's images, LanceDB 0.40.0 in a venv, the vectors, c1_strata built
#   vm.sh c1 ARGS...     this checkout synced in, c1_strata built, `c1.py ARGS` run, results copied back
# The VM starts at the beginning and stops on every exit. Results land in $C1_VM_RESULTS.
set -euo pipefail
V=${C1_VM:-$HOME/.scratch/vm-document-gate/vm.sh}
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
RES=${C1_VM_RESULTS:-$HOME/.scratch/c1-vm}
commit=$(git -C "$root" rev-parse --short=12 HEAD)
mkdir -p "$RES"
vm() { "$V" linux ssh "$@"; }
[ "$("$V" linux status)" = running ] && "$V" linux stop
# Whatever ended the run (a deadline's TERM included), the results and logs come back and the VM stops.
fetch() { vm 'tar -C ~/c1 -cf - results logs' 2>/dev/null | tar -C "$RES" -xf - || true; }
bye() { trap - EXIT TERM INT; fetch; "$V" linux stop; }
trap bye EXIT
trap 'exit 143' TERM INT
CPUS=8 MEM=16384 "$V" linux start
vm 'nproc; free -g | head -2; df -h / | tail -1'
git -C "$root" archive --prefix=c1src/ HEAD | vm 'rm -rf ~/c1src && tar -xf - -C ~'
case $1 in
setup)
	vm 'set -e; sudo apt-get update -q >/dev/null; sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q docker.io python3-venv cmake ninja-build >/dev/null
		sudo usermod -aG docker "$USER"; for i in qdrant/qdrant:v1.20.0 pgvector/pgvector:0.8.7-pg17 docker.elastic.co/elasticsearch/elasticsearch:9.5.5; do sudo docker pull -q $i; done
		mkdir -p ~/c1; [ -x ~/c1/venv/bin/python ] || python3 -m venv ~/c1/venv
		~/c1/venv/bin/pip install -q --only-binary :all: lancedb==0.40.0 numpy requests "psycopg[binary]"
		[ -s ~/c1/vec.f32 ] || C1_HOME=~/c1 ~/c1/venv/bin/python ~/c1src/bench/hybrid/c1.py gen
		docker --version; ~/c1/venv/bin/python -c "import lancedb; print(\"lancedb\", lancedb.__version__)"'
	;;
c1) ;;
*) echo "usage: vm.sh setup | c1 ARGS..." >&2; exit 2 ;;
esac
vm 'set -e; cmake -S ~/c1src/bench/hybrid -B ~/c1build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C ~/c1build c1_strata | tail -1'
if [ "$1" = c1 ]; then
	shift
	status=0
	vm "cd ~/c1src && C1_HOME=~/c1 C1_COMMIT=$commit C1_STRATA=~/c1build/c1_strata ~/c1/venv/bin/python bench/hybrid/c1.py $*" || status=$?
	exit $status
fi
