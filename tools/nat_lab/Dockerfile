# One image for every NAT lab container (tools/nat_lab/nat_lab.py): routers need
# iptables + conntrack, the "PCs" need the lobby's Python dependencies.
FROM python:3.12-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends iptables iproute2 procps conntrack \
 && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir pystun3 zstandard
