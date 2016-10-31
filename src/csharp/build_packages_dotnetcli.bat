#!/bin/bash
# Copyright 2016, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

@rem Current package versions
set VERSION=1.0.1
set PROTOBUF_VERSION=3.0.0

@rem Adjust the location of nuget.exe
set NUGET=C:\nuget\nuget.exe
set DOTNET="C:\Program Files\dotnet\dotnet"

set -ex

mkdir -p ../../artifacts/

@rem Collect the artifacts built by the previous build step if running on Jenkins
@rem TODO(jtattermusch): is there a better way to do this?
xcopy /Y /I ..\..\architecture=x86,language=csharp,platform=windows\artifacts\* nativelibs\windows_x86\
xcopy /Y /I ..\..\architecture=x64,language=csharp,platform=windows\artifacts\* nativelibs\windows_x64\
xcopy /Y /I ..\..\architecture=x86,language=csharp,platform=linux\artifacts\* nativelibs\linux_x86\
xcopy /Y /I ..\..\architecture=x64,language=csharp,platform=linux\artifacts\* nativelibs\linux_x64\
xcopy /Y /I ..\..\architecture=x86,language=csharp,platform=macos\artifacts\* nativelibs\macosx_x86\
xcopy /Y /I ..\..\architecture=x64,language=csharp,platform=macos\artifacts\* nativelibs\macosx_x64\

@rem Collect protoc artifacts built by the previous build step
xcopy /Y /I ..\..\architecture=x86,language=protoc,platform=windows\artifacts\* protoc_plugins\windows_x86\
xcopy /Y /I ..\..\architecture=x64,language=protoc,platform=windows\artifacts\* protoc_plugins\windows_x64\
xcopy /Y /I ..\..\architecture=x86,language=protoc,platform=linux\artifacts\* protoc_plugins\linux_x86\
xcopy /Y /I ..\..\architecture=x64,language=protoc,platform=linux\artifacts\* protoc_plugins\linux_x64\
xcopy /Y /I ..\..\architecture=x86,language=protoc,platform=macos\artifacts\* protoc_plugins\macosx_x86\
xcopy /Y /I ..\..\architecture=x64,language=protoc,platform=macos\artifacts\* protoc_plugins\macosx_x64\

%DOTNET% restore .

%DOTNET% pack --configuration Release Grpc.Core/project.json --output ../../artifacts
%DOTNET% pack --configuration Release Grpc.Auth/project.json --output ../../artifacts
%DOTNET% pack --configuration Release Grpc.HealthCheck/project.json --output ../../artifacts

%NUGET% pack Grpc.nuspec -Version "1.0.1" -OutputDirectory ../../artifacts
%NUGET% pack Grpc.Tools.nuspec -Version "1.0.1" -OutputDirectory ../../artifacts

@rem copy resulting nuget packages to artifacts directory
xcopy /Y /I *.nupkg ..\..\artifacts\

@rem create a zipfile with the artifacts as well
powershell -Command "Add-Type -Assembly 'System.IO.Compression.FileSystem'; [System.IO.Compression.ZipFile]::CreateFromDirectory('..\..\artifacts', 'csharp_nugets_obsolete.zip');"
xcopy /Y /I csharp_nugets_obsolete.zip ..\..\artifacts\

goto :EOF

:error
echo Failed!
exit /b %errorlevel%