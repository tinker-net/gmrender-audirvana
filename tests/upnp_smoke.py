#!/usr/bin/env python3
"""UPnP SOAP smoke tests for a dedicated, idle gmrender instance.

Run ONLY against a disposable test renderer (e.g. --gstout-audiosink=fakesink).
The tests modify transport and volume state. No audio source is played.
Uses only the Python standard library.
"""
import argparse
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

SOAP = "http://schemas.xmlsoap.org/soap/envelope/"
SERVICES = {
    "AVTransport": "rendertransport",
    "RenderingControl": "rendercontrol",
    "ConnectionManager": "renderconnmgr",
}
ET.register_namespace("s", SOAP)


def local(tag):
    return tag.rsplit("}", 1)[-1]


def check(actual, expected, label):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


class Renderer:
    def __init__(self, url):
        self.url = url.rstrip("/")

    def call(self, service, action, args=None, fault=None):
        ns = f"urn:schemas-upnp-org:service:{service}:1"
        env = ET.Element(f"{{{SOAP}}}Envelope")
        body = ET.SubElement(env, f"{{{SOAP}}}Body")
        command = ET.SubElement(body, f"{{{ns}}}{action}")
        for key, value in (args or {}).items():
            ET.SubElement(command, key).text = str(value)
        req = urllib.request.Request(
            f"{self.url}/upnp/control/{SERVICES[service]}1",
            data=ET.tostring(env, encoding="utf-8", xml_declaration=True),
            headers={"Content-Type": 'text/xml; charset="utf-8"',
                     "SOAPAction": f'"{ns}#{action}"'},
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                root = ET.fromstring(response.read())
        except urllib.error.HTTPError as error:
            root = ET.fromstring(error.read())
            codes = [n.text for n in root.iter() if local(n.tag) == "errorCode"]
            if fault is not None:
                check(codes, [str(fault)], f"{service}.{action} fault")
                return {}
            raise AssertionError(f"{service}.{action}: HTTP {error.code}, SOAP {codes}") from error
        if fault is not None:
            raise AssertionError(f"{service}.{action}: expected fault {fault}, received success")
        result = root.find(f".//{{{ns}}}{action}Response")
        if result is None:
            raise AssertionError(f"{service}.{action}: missing SOAP response")
        return {local(n.tag): n.text or "" for n in result}

    def scpd(self, service):
        with urllib.request.urlopen(
            f"{self.url}/upnp/{SERVICES[service]}SCPD.xml", timeout=10
        ) as response:
            root = ET.fromstring(response.read())
        actions = {}
        for action in root.iter():
            if local(action.tag) != "action":
                continue
            name = next(n.text for n in action if local(n.tag) == "name")
            args = {}
            for arg in action.iter():
                if local(arg.tag) == "argument":
                    fields = {local(n.tag): n.text for n in arg}
                    args[fields["name"]] = fields["direction"]
            actions[name] = args
        return actions


def run(renderer):
    avt = "AVTransport"
    rcs = "RenderingControl"
    cm = "ConnectionManager"
    instance = {"InstanceID": "0"}
    master = {**instance, "Channel": "Master"}

    def av(action, **kwargs):
        return renderer.call(avt, action, {**instance, **kwargs})

    def rc(action, **kwargs):
        return renderer.call(rcs, action, {**master, **kwargs})

    check(renderer.scpd(avt)["SetPlayMode"]["NewPlayMode"], "in", "SetPlayMode SCPD")
    check(renderer.scpd(rcs)["GetVolumeDB"]["CurrentVolume"], "out", "GetVolumeDB SCPD")
    check(renderer.call(cm, "GetCurrentConnectionIDs")["ConnectionIDs"], "0", "Connection IDs")
    assert "Sink" in renderer.call(cm, "GetProtocolInfo")
    av("SetPlayMode", NewPlayMode="NORMAL")
    check(av("GetTransportSettings")["PlayMode"], "NORMAL", "Play mode")
    check(av("GetTransportSettings")["RecQualityMode"], "NOT_IMPLEMENTED", "Record quality")
    assert {"PlayMedia", "RecMedia", "RecQualityModes"} <= av("GetDeviceCapabilities").keys()
    renderer.call(avt, "SetPlayMode", {**instance, "NewPlayMode": "SHUFFLE"}, fault=712)
    renderer.call(avt, "GetTransportInfo", {"InstanceID": "99"}, fault=718)

    try:
        av("Stop")
        av("SetAVTransportURI", CurrentURI="", CurrentURIMetaData="")
        uri = "http://127.0.0.1:9/unplayed.wav"
        av("SetAVTransportURI", CurrentURI=uri, CurrentURIMetaData="")
        check(av("GetMediaInfo")["CurrentURI"], uri, "Current URI")
        check(av("GetMediaInfo")["PlayMedium"], "NETWORK", "Playback medium")
        av("SetNextAVTransportURI", NextURI=uri, NextURIMetaData="")
        check(av("GetMediaInfo")["NextURI"], uri, "Next URI")
        av("SetNextAVTransportURI", NextURI="", NextURIMetaData="")
        check(av("GetMediaInfo")["NextURI"], "", "Empty next URI")
        av("SetNextAVTransportURI", NextURI=uri, NextURIMetaData="")
        av("SetAVTransportURI", CurrentURI="", CurrentURIMetaData="")
        media = av("GetMediaInfo")
        check(media["CurrentURI"], "", "Cleared current URI")
        check(media["NextURI"], "", "Cleared next URI")
        check(media["NrTracks"], "0", "Cleared track count")
        check(av("GetTransportInfo")["CurrentTransportState"], "STOPPED", "Cleared state")
        check(av("GetCurrentTransportActions")["Actions"], "", "Cleared actions")

        check(rc("GetVolumeDBRange"), {"MinValue": "-15360", "MaxValue": "0"}, "dB range")
        rc("SetVolume", DesiredVolume="100")
        check(rc("GetVolume")["CurrentVolume"], "100", "Scalar volume")
        check(rc("GetVolumeDB")["CurrentVolume"], "0", "Unity gain")
        rc("SetVolumeDB", DesiredVolume="-5120")
        check(rc("GetVolume")["CurrentVolume"], "50", "-20 dB scalar")
        check(rc("GetVolumeDB")["CurrentVolume"], "-5120", "-20 dB units")
        rc("SetMute", DesiredMute="true")
        check(rc("GetMute")["CurrentMute"], "1", "Mute")
        rc("SetMute", DesiredMute="false")
        check(rc("GetMute")["CurrentMute"], "0", "Unmute")
        renderer.call(rcs, "SetVolume", master, fault=402)
    finally:
        av("Stop")
        av("SetAVTransportURI", CurrentURI="", CurrentURIMetaData="")
    print("PASS: UPnP SOAP smoke tests")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url", help="e.g. http://192.168.1.10:49494")
    parser.add_argument("--test-instance", action="store_true",
                        help="confirm the instance is disposable and idle")
    args = parser.parse_args()
    if not args.test_instance:
        parser.error("--test-instance is required: this changes renderer state")
    run(Renderer(args.base_url))
