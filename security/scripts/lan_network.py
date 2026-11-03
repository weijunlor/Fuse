"""Shared IPv4/interface selection for secure launchers and LAN diagnostics."""
import ipaddress
import json
import os
import subprocess
import xml.etree.ElementTree as ET


def ipv4(value):
    address = ipaddress.IPv4Address(value)
    if address.is_unspecified or address.is_multicast or str(address) == "255.255.255.255":
        raise ValueError("server must be a unicast IPv4 address")
    return str(address)


def ip_json(*args):
    result = subprocess.run(["ip", "-j", "-4", *args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"Cannot inspect network: {result.stderr.strip()}")
    return json.loads(result.stdout)


def interfaces():
    return {row["ifname"]: [item["local"] for item in row.get("addr_info", [])
                            if item.get("family") == "inet"]
            for row in ip_json("address", "show")}


def select_interface(server=None, interface=None):
    available = interfaces()
    if interface:
        if interface not in available or not available[interface]:
            raise ValueError(f"Interface {interface!r} has no IPv4 address")
        return interface
    if server:
        server = ipv4(server)
        # Linux routes this computer's own LAN addresses through lo. DDS must
        # advertise the adapter that owns the address, matching the host's LAN
        # participant, rather than switching a same-computer client to loopback.
        for name, addresses in available.items():
            if server in addresses:
                return name
        routes = ip_json("route", "get", server)
        if not routes or not routes[0].get("dev"):
            raise RuntimeError(f"No route to {server}")
        return routes[0]["dev"]
    # A host has no destination to route to yet. Prefer its active default
    # route so DDS advertises the same LAN adapter as the pairing listener.
    # Interface numbers vary across WSL machines and can change after restart.
    usable = {name for name, addresses in available.items() if name != "lo" and any(
        not (address.is_loopback or address.is_link_local or address.is_unspecified or
             address.is_multicast or str(address) == "255.255.255.255")
        for address in map(ipaddress.IPv4Address, addresses))}
    routes = sorted(ip_json("route", "show", "default"), key=lambda row: row.get("metric", 0))
    for route in routes:
        if route.get("dev") in usable and "linkdown" not in route.get("flags", []):
            return route["dev"]
    if len(usable) == 1:
        return next(iter(usable))  # An isolated LAN may have no default gateway.
    return None  # Ambiguous/no LAN: retain DDS selection; pairing requests an override.


def networking_mode():
    try:
        result = subprocess.run(["wslinfo", "--networking-mode"], capture_output=True,
                                text=True, timeout=5)
        if result.returncode == 0:
            return result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    # Older WSL lacks wslinfo; do not mistake it for native Linux.
    with open("/proc/sys/kernel/osrelease", encoding="ascii") as source:
        return "unknown (WSL)" if "microsoft" in source.read().lower() else "native Linux"


def configure(domain, identity):
    server = os.environ.get("FUSE_SERVER_IP") if identity != "server" else None
    server = ipv4(server) if server else None
    interface = select_interface(server, os.environ.get("FUSE_DDS_INTERFACE"))
    general = ET.SubElement(domain, "General")
    # Cyclone 0.10.5 calls IPv4 UDP "udp" (not "udp4").
    ET.SubElement(general, "Transport").text = "udp"
    ET.SubElement(general, "AllowMulticast").text = (
        "false" if server or os.environ.get("FUSE_DDS_MULTICAST") == "off" else "spdp")
    if interface:
        entries = ET.SubElement(general, "Interfaces")
        ET.SubElement(entries, "NetworkInterface", name=interface)
    discovery = ET.SubElement(domain, "Discovery")
    # Bounded indices make both unicast discovery and firewall rules predictable.
    ET.SubElement(discovery, "ParticipantIndex").text = "auto"
    ET.SubElement(discovery, "MaxAutoParticipantIndex").text = "9"
    if server:
        peers = ET.SubElement(discovery, "Peers")
        ET.SubElement(peers, "Peer", Address=server)
    return server, interface
