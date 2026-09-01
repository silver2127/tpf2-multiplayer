#!/usr/bin/env python3
"""
swarm.py -- peer-to-peer save distribution for the netpunch lobby.

The star transfer (lobby.py ``_HostSaveTransfer``) uploads the whole save once
PER JOINER, so the host's upload link is the ceiling. Here the save is split
into PIECES and every node -- host and joiners -- serves the pieces it holds to
whoever asks, so the host uploads roughly one copy in