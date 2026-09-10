#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

import click
import ipaddress
import iptc
from pyroute2 import IPRoute
from pyroute2.netlink import NetlinkError


def handle_ip_string(ctx, param, value):
    try:
        ret = ipaddress.ip_network(value)
        return ret
    except ValueError:
        raise click.BadParameter(f'{value} is not a valid IP range.')


def iptables_add_masquerade(if_name, ip_range):
    chain = iptc.Chain(iptc.Table(iptc.Table.NAT), "POSTROUTING")
    rule = iptc.Rule()
    rule.src = ip_range
    rule.out_interface = if_name
    target = iptc.Target(rule, "MASQUERADE")
    rule.target = target
    chain.insert_rule(rule)


def iptables_allow_all(if_name):
    chain = iptc.Chain(iptc.Table(iptc.Table.FILTER), "INPUT")
    rule = iptc.Rule()
    rule.in_interface = if_name
    target = iptc.Target(rule, "ACCEPT")
    rule.target = target
    chain.insert_rule(rule)


@click.command()
@click.option("--if_name", default="ogstun", help="TUN interface name.")
@click.option("--ip_range", default='10.45.0.0/24', callback=handle_ip_string,
              help="IP range of the TUN interface.")
def main(if_name, ip_range):

    first_ip_addr = next(ip_range.hosts(), None)
    if first_ip_addr is None:
        raise ValueError('Invalid IP range.')

    ipr = IPRoute()
    try:
        ipr.link('add', ifname=if_name, kind='tuntap', mode='tun')
    except NetlinkError:
        pass

    links = ipr.link_lookup(ifname=if_name)
    if not links:
        raise RuntimeError(f'Unable to create TUN interface {if_name}.')
    dev = links[0]
    ipr.link('set', index=dev, state='down')
    try:
        ipr.addr('add', index=dev, address=first_ip_addr.exploded, mask=ip_range.prefixlen)
    except NetlinkError:
        pass
    ipr.link('set', index=dev, state='up')

    iptables_add_masquerade(if_name, ip_range.with_prefixlen)
    iptables_allow_all(if_name)


if __name__ == "__main__":
    main()