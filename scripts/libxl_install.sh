#!/bin/bash
make -C tools subdir-install-libxl
mkdir -p /var/lib/xen

cp xen-init-dom0.initscript /etc/init.d/xen-init-dom0
cp xl.conf /etc/xen/xl.conf
cp xen-init-br.sh /etc/init.d/xen-init-br.sh

chmod 0755 /etc/init.d/xen-init-dom0
chmod 0644 /etc/xen/xl.conf
chmod 0755 /etc/init.d/xen-init-br.sh

update-rc.d xen-init-br.sh defaults
update-rc.d xen-init-dom0 defaults
