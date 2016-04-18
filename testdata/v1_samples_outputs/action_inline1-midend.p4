#include "/home/cdodd/p4c/build/../p4include/core.p4"
#include "/home/cdodd/p4c/build/../p4include/v1model.p4"

header data_t {
    bit<32> f1;
    bit<32> f2;
    bit<32> f3;
    bit<32> f4;
    bit<8>  b1;
    bit<8>  b2;
    bit<16> h1;
}

struct metadata {
}

struct headers {
    @name("data") 
    data_t data;
}

parser ParserImpl(packet_in packet, out headers hdr, inout metadata meta, inout standard_metadata_t standard_metadata) {
    @name("start") state start {
        packet.extract(hdr.data);
        transition accept;
    }
}

control ingress(inout headers hdr, inout metadata meta, inout standard_metadata_t standard_metadata) {
    @name("setb1") action setb1(bit<8> val, bit<9> port) {
        bool hasReturned_0 = false;
        @name("dest_0") bit<8> dest_0_0;
        @name("val_0") bit<8> val_0_0;
        {
            dest_0_0 = hdr.data.b1;
            val_0_0 = val;
            dest_0_0 = val_0_0;
            hdr.data.b1 = dest_0_0;
        }
        standard_metadata.egress_spec = port;
    }
    @name("noop") action noop() {
        bool hasReturned_1 = false;
    }
    @name("test1") table test1() {
        actions = {
            setb1;
            noop;
            NoAction;
        }
        key = {
            hdr.data.f1: ternary;
        }
        default_action = NoAction();
    }

    apply {
        bool hasExited = false;
        test1.apply();
    }
}

control egress(inout headers hdr, inout metadata meta, inout standard_metadata_t standard_metadata) {
    apply {
        bool hasExited_0 = false;
    }
}

control DeparserImpl(packet_out packet, in headers hdr) {
    apply {
        bool hasExited_1 = false;
        packet.emit(hdr.data);
    }
}

control verifyChecksum(in headers hdr, inout metadata meta, inout standard_metadata_t standard_metadata) {
    apply {
        bool hasExited_2 = false;
    }
}

control computeChecksum(inout headers hdr, inout metadata meta) {
    apply {
        bool hasExited_3 = false;
    }
}

V1Switch(ParserImpl(), verifyChecksum(), ingress(), egress(), computeChecksum(), DeparserImpl()) main;
