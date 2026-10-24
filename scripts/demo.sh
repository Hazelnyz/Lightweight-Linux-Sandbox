#!/bin/bash
# minibox demo: limited vs unlimited runs for memory, fork and CPU
cd "$(dirname "$0")/.." || exit 1
mkdir -p results
sudo rm -f results/*.csv results/*.txt

run() {   # usage: run <name> <minibox options...> <command...>
  name=$1; shift
  sudo rmdir /sys/fs/cgroup/minibox/"$name" 2>/dev/null
  echo; echo "=================== $name ==================="
  sudo ./minibox run --name "$name" "$@" 2>&1 | tee results/"$name".txt | grep -v '^allocated'
  [ -f minibox-"$name".csv ] && sudo mv minibox-"$name".csv results/
}

run mem_limited    --mem 64M  /bin/stress mem 300
run mem_unlimited             /bin/stress mem 300
run fork_limited   --pids 20  /bin/stress fork 100
run fork_unlimited            /bin/stress fork 100
run cpu_limited    --cpus 0.5 /bin/sh -c "timeout 5 /bin/stress cpu; true"
run cpu_unlimited             /bin/sh -c "timeout 5 /bin/stress cpu; true"

sudo chown -R "$(id -un):$(id -gn)" results
echo; echo "Done. Files in results/:"; ls -l results
