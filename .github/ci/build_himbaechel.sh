#!/bin/bash


export XRAY_DB_PATH=${DEPS_PATH}/prjxray-db
export PRJXRAY=${DEPS_PATH}/prjxray
export PRJXRAY_DB=${XRAY_DB_PATH}
export PEPPERCORN_PATH=${DEPS_PATH}/prjpeppercorn


function get_dependencies {
    # Fetch prjxray-db
    git clone https://github.com/openXC7/prjxray-db ${XRAY_DB_PATH}
    # Fetch apycula
    pip install --break-system-packages apycula
    # Fetch prjpeppercorn
    git clone https://github.com/YosysHQ/prjpeppercorn ${PEPPERCORN_PATH}
}

function build_nextpnr {
    mkdir build
    pushd build
    cmake .. -DARCH=himbaechel -DHIMBAECHEL_UARCH="gowin;xilinx;example;gatemate" -DHIMBAECHEL_EXAMPLE_DEVICES=example \
        -D HIMBAECHEL_XILINX_DEVICES="xc7a50t" -D HIMBAECHEL_PRJXRAY_DB=${XRAY_DB_PATH} \
        -D HIMBAECHEL_GOWIN_DEVICES="GW1N-9C;GW5A-25A" \
        -D HIMBAECHEL_PEPPERCORN_PATH=${PEPPERCORN_PATH}
    make nextpnr-himbaechel -j`nproc`
    popd
}

function run_tests {
    export PATH=${GITHUB_WORKSPACE}/.yosys/bin:${PATH}
    export PYTHONPATH=${PRJXRAY}:${PYTHONPATH}
    pushd himbaechel/uarch/xilinx/examples/arty-a35
    yosys -p "synth_xilinx -flatten -abc9 -nobram -arch xc7 -top top; write_json blinky.json" blinky.v
    ../../../../build/nextpnr-himbaechel --device xc7a35tcsg324-1 -o xdc=arty.xdc --json blinky.json -o fasm=blinky.fasm --router router2
    popd
    pushd himbaechel/uarch/xilinx/examples/sonata
    make DESIGN=johnson_sonata TOP=johnson_sonata XDC=johnson_sonata.xdc uf2
    popd
}

function run_archcheck {
    pushd build
    ./nextpnr-himbaechel --device EXAMPLE --test
    popd
}
