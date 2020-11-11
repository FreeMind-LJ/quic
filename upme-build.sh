#!/bin/bash
# upme-build.sh
# parameters must to handle:
# @release - $1
# @install_dir - $2
#
# return value:
# 0 for success
# other for failed

if [ "$#" -ne 2 ]; then
    echo "Must enter 2 command line arguments: \$1 for release and \$2 for install_dir"
    exit 1
fi

release=$1
install_dir=$2

version=1.0.0
package=quicserver
target=$package-$version
target_debug=$package-debuginfo-$version

cp -rp . ~/rpmbuild/SOURCES/$target
cd ~/rpmbuild/SOURCES && tar zcf $target.tar.gz $target
rm -rf ~/rpmbuild/SOURCES/$target

cd -

sed -e "s,@version@,${version},g" -e "s,@release@,${release},g" ${package}.spec.in > ${package}.spec
rpmbuild -bb --clean ${package}.spec

# move RPMs into install directory
mv ~/rpmbuild/RPMS/x86_64/${target}-${release}*.x86_64.rpm ${install_dir}/
mv ~/rpmbuild/RPMS/x86_64/${target_debug}-${release}*.x86_64.rpm ${install_dir}/
