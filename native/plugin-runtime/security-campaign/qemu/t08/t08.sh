#!/bin/bash
# T08 connection-time network policy harness (guest init).
# Brings up local "public" (1.1.1.1) and "private" (10.99.0.1) loopback
# addresses, maps approved/private hostnames via /etc/hosts, points the
# provider's TLS trust at a freshly generated CA, then runs the driver.
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mount -t tmpfs tmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null
hostname t08

# Local addresses: 1.1.1.1 is treated as public by the provider, 10.99.0.1 is
# RFC1918/private.
ip addr add 127.0.0.1/8 dev lo 2>/dev/null
ip link set lo up
ip addr add 1.1.1.1/32 dev lo 2>/dev/null
ip addr add 10.99.0.1/32 dev lo 2>/dev/null

cat > /etc/hosts <<'EOF'
127.0.0.1   localhost
::1         localhost
1.1.1.1     app.example
1.1.1.1     stream.example
10.99.0.1   leak.example
10.99.0.1   privatestream.example
EOF

# TLS trust for the positive-control HTTPS fetch (curl reads SSL_CERT_FILE).
export SSL_CERT_FILE=/root/ca.crt

cd /root
python3 t08_test.py
rc=$?
echo "T08-DRIVER-EXIT=$rc"
# Halt and query for the marker on the host.
echo "T08-DONE"
sync
poweroff -f 2>/dev/null || /sbin/poweroff -f 2>/dev/null || true
exit 0
