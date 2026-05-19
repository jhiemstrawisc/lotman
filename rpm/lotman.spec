#############################################
# Global macros that can be used throughout #
#############################################
%global srcname lotman

Name: %srcname
Version: 0.1.0
Release: 1%{?dist}
Summary: C++ Implementation of the LotMan Library
License: Apache-2.0
URL: https://github.com/PelicanPlatform/lotman

Source0: https://github.com/PelicanPlatform/lotman/releases/download/v%{version}/lotman-%{version}.tar.gz

#############################################
# Build dependencies                        #
#############################################
BuildRequires: gcc-c++
BuildRequires: make
BuildRequires: cmake3 >= 3.18.4
BuildRequires: cmake-rpm-macros
BuildRequires: sqlite-devel
BuildRequires: libuuid-devel
BuildRequires: nlohmann-json-devel
BuildRequires: json-schema-validator-devel

%description
Public headers for the LotMan library, which tracks data usage in dHTC environments over the "lot" object.

#############################################
# To suppress some of the debug outputs     #
#############################################
%global debug_package %{nil}

#############################################
# RHEL 9 will try to build out of source,   #
# so that needs to be overridden.           #
#############################################
%if 0%{?rhel} > 8
%global __cmake_in_source_build 1
%endif

#############################################
# Beginning of the build + make workflow    #
#############################################
%prep
%autosetup -n %{srcname}-%{version}

%build
mkdir -p build
cd build
%cmake ..
%make_build

%install
cd build
%make_install

%files
%license LICENSE
%doc README.md
#%{_libdir}/libLotMan.so.0*
%{_libdir}/libLotMan.so
#%{_bindir}/lotman-*
%{_includedir}/lotman/lotman.h
%dir %{_includedir}/lotman

%changelog
* Tue May 19 2026 Justin Hiemstra <jhiemstra@wisc.edu> - 0.1.0-1
- Significantly extend APIs to include query time arguments for lot timeline filtering
- Introduce strict hierarchy mode and reservation semantics
- Allow multiple lots to claim similar path if they don't intersect in time
- Add connection pooling in the database layer

* Wed Sep 18 2024 Justin Hiemstra <jhiemstra@wisc.edu> - 0.0.4-1
- Add RPM specfile to project

* Wed Sep 18 2024 Justin Hiemstra <jhiemstra@wisc.edu> - 0.0.3-1
- Fixes for paths JSON data structure.
- Error message cleanups.
- Various unit test upgrades.

* Wed Oct 18 2023 Justin Hiemstra <jhiemstra@wisc.edu> - 0.0.2-1
- v0.0.2 release of LotMan -- adds the ability to set sqlite timeouts
  and enables WAL mode for better concurrency.

* Tue Jun 27 2023 Justin Hiemstra <jhiemstra@morgridge.org> - 0.0.1-1
- Initial release of the LotMan C++ RPM.
