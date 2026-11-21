#!/bin/sh
# Installs C1's baselines natively under $C1_HOME (default: this worktree's .scratch/c1), nothing
# system-wide: Qdrant and Elasticsearch release binaries, PostgreSQL and pgvector built from
# source, LanceDB in a venv. Then generates the shared vectors. Idempotent: present parts are kept.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
H=${C1_HOME:-$(cd "$here/../.." && pwd)/.scratch/c1}
QDRANT=1.20.2 ES=9.5.4 PG=17.11 PGVECTOR=0.8.7 LANCEDB=0.25.3
mkdir -p "$H/dl"
cd "$H"
case $(uname -m) in x86_64) arch=x86_64 ;; arm64) arch=aarch64 ;; *) echo "unsupported arch"; exit 1 ;; esac
get() { [ -s "dl/$2" ] || curl -fsSL -o "dl/$2" "$1"; }

if [ ! -x qdrant/qdrant ]; then
	get "https://github.com/qdrant/qdrant/releases/download/v$QDRANT/qdrant-$arch-apple-darwin.tar.gz" qdrant.tgz
	mkdir -p qdrant && tar -xzf dl/qdrant.tgz -C qdrant
fi
if [ ! -x "es/bin/elasticsearch" ]; then
	get "https://artifacts.elastic.co/downloads/elasticsearch/elasticsearch-$ES-darwin-$arch.tar.gz" es.tgz
	rm -rf es "elasticsearch-$ES" && tar -xzf dl/es.tgz && mv "elasticsearch-$ES" es
fi
if [ ! -x pg/bin/postgres ]; then
	get "https://ftp.postgresql.org/pub/source/v$PG/postgresql-$PG.tar.bz2" pg.tbz
	rm -rf "src-pg" && mkdir src-pg && tar -xjf dl/pg.tbz -C src-pg --strip-components 1
	(cd src-pg && ./configure --prefix="$H/pg" --without-icu --without-readline --without-zlib >/dev/null && make -s -j8 >/dev/null && make -s install >/dev/null)
	rm -rf src-pg
fi
if [ ! -f pg/lib/postgresql/vector.dylib ] && [ ! -f pg/lib/vector.dylib ]; then
	get "https://github.com/pgvector/pgvector/archive/refs/tags/v$PGVECTOR.tar.gz" pgvector.tgz
	rm -rf src-pgvector && mkdir src-pgvector && tar -xzf dl/pgvector.tgz -C src-pgvector --strip-components 1
	(cd src-pgvector && make -s PG_CONFIG="$H/pg/bin/pg_config" >/dev/null && make -s PG_CONFIG="$H/pg/bin/pg_config" install >/dev/null)
	rm -rf src-pgvector
fi
if [ ! -x venv/bin/python ]; then
	py=$(ls /usr/local/bin/python3.12 2>/dev/null || command -v python3)
	"$py" -m venv venv
fi
venv/bin/python -c 'import lancedb, numpy, requests, psycopg' 2>/dev/null ||
	venv/bin/pip install -q --only-binary :all: "lancedb==$LANCEDB" numpy requests 'psycopg[binary]'
rm -rf dl
[ -s vec.f32 ] || venv/bin/python "$here/c1.py" gen
echo "setup: $H"
qdrant/qdrant --version; es/bin/elasticsearch --version 2>/dev/null | head -1 || true
pg/bin/postgres --version; venv/bin/python -c 'import lancedb; print("lancedb", lancedb.__version__)'
