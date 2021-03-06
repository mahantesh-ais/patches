# patches
Patches for xen-4.8.1 to integrate blktap3, Scripts to help make the installation easier

Steps followed to get Xen and Blktap3 working in coordination.

1. Install jessie, update the linux kernel to 4.9 or later. This is important as blktap3 uses data structures from linux headers as per the recent release. (My Update was done using linux backports https://backports.debian.org/Instructions/ ).

	`sudo apt-get -t jessie-backports install "linux-image-4.9.0-0.bpo.3-amd64"`

	`sudo apt-get -t jessie-backports install "linux-headers-4.9.0-0.bpo.3-amd64"`

	`sudo apt-get -t jessie-backports install linux-libc-dev`

	`sudo reboot`

2. Download and install dkms-blktap-2.0.93-0.9 or higher (https://packages.debian.org/sid/kernel/blktap-dkms).
	>Prerequisites: dkms
	
	`sudo apt-get install dkms`

3. (Note: Don't download xen from the git repo mentioned in the below link as blktap3 requires xen-4.8 or higher), only Download and install all the dependencies for xen (along with 'libssl-dev') given in the xen-wiki page:
	https://wiki.xenproject.org/wiki/Compiling_Xen_From_Source

	`sudo apt-get install build-essential bcc bin86 gawk bridge-utils iproute libcurl3 libcurl4-openssl-dev bzip2 module-init-tools transfig texinfo texlive-latex-base texlive-latex-recommended texlive-fonts-extra texlive-fonts-recommended pciutils-dev mercurial make gcc libc6-dev zlib1g-dev python python-dev python-twisted libncurses5-dev patch libvncserver-dev libsdl-dev libjpeg62-turbo-dev iasl libbz2-dev e2fslibs-dev git-core uuid-dev ocaml ocaml-findlib libx11-dev bison flex xz-utils libyajl-dev gettext libpixman-1-dev libaio-dev markdown pandoc libc6-dev-i386 libssl-dev autoconf automake libtool`

	`sudo apt-get build-dep xen`

 	Now download xen-4.8.1 or xen-4.9.0 (it is referred as xen-blktap3 henceforth in the document). Apply all the patches in xen-4.X.X/patches and download the scripts from xen-4.X.X/scripts directory and place them inside xen-blktap3 tree.

	> Create file .config in the xen-blktap3 tree and insert the following line (without quotes). This is required while running pv guests for the pygrub to execute properly.
	
	> "PYTHON_PREFIX_ARG=--install-layout=deb"

	Run the following commands inside xen to build and install the dependencies for blktap3:

	`./configure --prefix=/usr`

	`./generic.sh`

	`sudo ./generic_install.sh`

4. Now, clone blktap3 outside xen-blktap3 tree from git repo https://github.com/mahantesh-ais/blktap3
	Run the following commands inside blktap3 tree to build and install it.

	`./autogen.sh`

	`./configure --prefix=/usr`

	`make`

	`sudo make install`

5. Download the scripts directory from this repository and move all the scripts inside xen-blktap3 and run the below commands.
	`./libxl.sh`

	`sudo ./libxl_install.sh`

	`sudo update-grub`

	> Reboot now(`sudo reboot`) and login to Xen hypervisor.

6. Run (as root) tapback as a daemon `tapback`, if debug messages are to be displayed then use `tapback -d -v`, these debug messages to the screen are added by me(Mahantesh Salimath). Some of the other logs can be found in /var/log/ directory.

Everything is set-up now :), need to start the guest to see xen and blktap3 in action.

PV guest:
	Download pv_guests/ to your working directory and also download the image files from *pv_images* (https://github.com/mahantesh-ais/pv_images) repository. Update the jessie-pv.cfg file with correct attributes for different parameters and run `sudo xl create jessie-pv.cfg`. If everything went correct you can see the new domain listed by running `sudo xl list`, you can login using `sudo xl console 'domid'`.

HVM guest:
	Will be updated soon...
