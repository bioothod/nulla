Summary:	Nulla Elliptics streaming server
Name:		nulla
Version:	0.1.2
Release:	1%{?dist}

License:	GPL 3.0
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
#%{_libdir}/*.so.*

%changelog
* Sat May 28 2016 Evgeniy Polyakov <zbr@ioremap.net> - 0.1.2
- sample: parse and store whole track metadata each time parse_track() is called, since it updates video frame rate and possibly other fields
- extract_meta: try stream reader, if it fails, use whole-file reader
- sample: serialize track as array, this will allow us to extend it by changing embedded version number
- iso_reader: added streaming ISO reader which accepts data chunks
- license: added copyright note
- Use GPL3 license
- upload: fixed CORS by setting Access-Control-Allow-Origin header to wildcard '*'
- config: log some headers
- server: implemented /upload/ handler which selects bucket and uploads data reading data in chunks, it returns json object which tells where and how file has been stored
- log: moved logger into own header
- index.html: added links to initialize stream if autodetection failed

* Tue Mar 08 2016 Evgeniy Polyakov <zbr@ioremap.net> - 0.1.1
- hls: generate correct playlist even if there are no audio/video tracks
- package: fixed debian/rpm builds

* Tue Mar 08 2016 Evgeniy Polyakov <zbr@ioremap.net> - 0.1.0
- Implemented runtime mpeg2ts container generation and HLS streaming

* Fri Feb 05 2016 Evgeniy Polyakov <zbr@ioremap.net> - 2.26.0.0.1
- Initial commit

