Summary:	Nulla Elliptics streaming server
Name:		nulla
Version:	0.0.1
Release:	1%{?dist}

License:	Apache 2.0
Group:		System Environment/Libraries
URL:		https://github.com/bioothod/nulla
Source0:	%{name}-%{version}.tar.bz2
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires:	boost-devel, boost-system, boost-thread
BuildRequires:	elliptics-client-devel >= 2.26.10.1
BuildRequires:  cmake, libthevoid3-devel >= 3.3.0, msgpack-devel, python-virtualenv
BuildRequires:	gpac-devel >= 0.6.0

%description
Nulla is an adaptive MPEG-DASH/HLS streaming server for Elliptics distributed storage.
It can stream the same files from elliptics storage either in DASH or HLS format without
repackaging or transcoding, it will generate container in runtime depending on the client.

Nulla allows channel muxing, you can stream 5 seconds from file X, then 10 seconds from file Y and so on.
For more details check http://video.reverbrain.com/index.html

%prep
%setup -q

%build
%{cmake} .

make %{?_smp_mflags}
#make test

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig


%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
#%doc README.md
%doc conf/*
%{_bindir}/*
%{_libdir}/*.so.*

%changelog
* Fri Feb 05 2016 Evgeniy Polyakov <zbr@ioremap.net> - 2.26.0.0.1
- Initial commit

