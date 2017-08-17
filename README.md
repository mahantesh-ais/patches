# patches
Patches for xen-4.8.1 to integrate blktap3, Scripts to help make the installation easier

Steps followed to get Xen and Blktap3 working in coordination.

1. Install jessie from bmo/internet, if the linux version is old one, update it to the latest version. This is important as blktap3 uses data structures in the linux headers as per the recent release. (My Update was done using linux backports https://backports.debian.org/Instructions/ ).
	1.`sudo apt-get -t jessie-backports install "linux-image-4.9.0-0.bpo.3-amd64"`
	2.`sudo apt-get -t jessie-backports install "linux-headers-4.9.0-0.bpo.3-amd64"`
	3.`sudo apt-get -t jessie-backports install linux-libc-dev`
	4.`sudo reboot`

2. Download(don't install) dkms-blktap-2.0.93-0.8, there is a bug in this package for debian, which will be fixed in the next release. If the next release is available, then download and install that instead of the above mentioned version. However, as of now next release is not available, hence replace the file  device.c in the dkms-blktap tree with blktap2_files/device.c in this repository. Build and install the blktap-2.0.93 now (Google how to build and install dkms packages, if you don't know already). 
	NOTE: Goto https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=870007 to read about bug discussed above.

3. (Note: Don't download xen from the git repo mentioned in the below link), only Download and install all the dependencies for xen (along with 'libssl-dev') given in the xen-wiki page:
	https://wiki.xenproject.org/wiki/Compiling_Xen_From_Source

	`sudo apt-get install build-essential bcc bin86 gawk bridge-utils iproute libcurl3 libcurl4-openssl-dev bzip2 module-init-tools transfig texinfo texlive-latex-base texlive-latex-recommended texlive-fonts-extra texlive-fonts-recommended pciutils-dev mercurial make gcc libc6-dev zlib1g-dev python python-dev python-twisted libncurses5-dev patch libvncserver-dev libsdl-dev libjpeg62-turbo-dev iasl libbz2-dev e2fslibs-dev git-core uuid-dev ocaml ocaml-findlib libx11-dev bison flex xz-utils libyajl-dev gettext libpixman-1-dev libaio-dev markdown pandoc libc6-dev-i386 libssl-dev autoconf libtool`

	`sudo apt-get build-dep xen`

 	Now download xen-4.8.1 from internet (it is referred as xen-blktap3 below).
	>> Create file .config in the xen-blktap3 tree and insert the following line (without quotes). This is required while running pv guests for the pygrub to execute properly.
	"PYTHON_PREFIX_ARG=--install-layout=deb"

	Run the following commands inside xen to build and install the dependencies for blktap3:
	a. `./configure --prefix=/usr`
	b. `./generic.sh`
	c. `sudo ./generic_install.sh`

4. Now, clone blktap3 outside xen-blktap3 tree from git repo https://github.com/mahantesh-ais/blktap3
	Run the following commands inside blktap3 tree to build and install it.

	a. ./autogen.sh (make sure to give permissions if you dont have)
	b. `./configure --prefix=/usr`
	c. `make`
	d. `sudo make install`

5. Download the scripts directory from this repository and move all the scripts inside xen-blktap3 and run the below commands.
	a. `./libxl.sh`
	b. `sudo ./libxl_install.sh`
	c. `./helper.sh`
	d. `sudo ./helper_install.sh`
	e. `make -C xen`
	f. `sudo make install-xen`
	g. `sudo update-grub`

----->Reboot now(`sudo reboot`) and load xen hypervisor.

6. Run (as root) tapback as a daemon `tapback -d`, if debug messages are to be displayed then use `tapback -d -v`, these debug messages to the screen are added by me(Mahantesh Salimath). Some of the other logs can be found in /var/log/ directory.

Everything is set-up now :), need to start the guest to see xen and blktap3 in action.

PV guest:
	1. Download pv_guests/ to your working directory, update the jessie-pv.cfg file with correct path for different parameters and run `xl create jessie-pv.cfg`. This should initiate creation of jessie-pv guest.

HVM guest:
	Will be updated soon...
