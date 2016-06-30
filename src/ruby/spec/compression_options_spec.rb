# Copyright 2015, Google Inc.
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

require 'grpc'

describe GRPC::Core::CompressionOptions do
  IDENTITY = 0
  DEFLATE = 1
  GZIP = 2

  GZIP_BIT = 1 << GZIP
  DEFLATE_BIT = 1 << DEFLATE
  IDENTITY_BIT = 1 << IDENTITY

  before(:example) do
    @compression_options = GRPC::Core::CompressionOptions.new
  end

  describe '#enable_algorithms' do
    it 'should pass correct internal values to #internal_enable_algorithms' do
      expect(@compression_options).to receive(:enable_algorithm_internal)
        .with(GZIP)
      @compression_options.enable_algorithms(:gzip)
    end

    it 'should pass the correct internal values with multiple parameters' do
      expect(@compression_options).to receive(:enable_algorithm_internal)
        .with(GZIP)
      expect(@compression_options).to receive(:enable_algorithm_internal)
        .with(DEFLATE)
      @compression_options.enable_algorithms(:gzip, :deflate)
    end
  end

  describe '#disable_algorithms' do
    it 'should pass correct internal values to #internal_disable_algorithms' do
      expect(@compression_options).to receive(:disable_algorithm_internal)
        .with(GZIP)
      @compression_options.disable_algorithms(:gzip)
    end

    it 'should work with more than one parameter' do
      expect(@compression_options).to receive(:disable_algorithm_internal)
        .with(GZIP)
      expect(@compression_options).to receive(:disable_algorithm_internal)
        .with(DEFLATE)
      @compression_options.disable_algorithms(:gzip, :deflate)
    end
  end

  it 'should provide the correct bit set for the enabled algorithms' do
    @compression_options.enable_algorithms(:gzip, :deflate)
    expect(@compression_options.enabled_algorithms_bitset).to eql(
      GZIP_BIT | DEFLATE_BIT | IDENTITY_BIT)

    @compression_options.disable_algorithms(:gzip)
    expect(@compression_options.enabled_algorithms_bitset).to eql(
      DEFLATE_BIT | IDENTITY_BIT)

    @compression_options.disable_algorithms(:deflate)
    expect(@compression_options.enabled_algorithms_bitset).to eql(IDENTITY_BIT)

    @compression_options.enable_algorithms(:gzip, :deflate)
    expect(@compression_options.enabled_algorithms_bitset).to eql(
      GZIP_BIT | DEFLATE_BIT | IDENTITY_BIT)
  end

  it 'should be able to set the default algorithm' do
    @compression_options.default_algorithm = :gzip
    expect(@compression_options.default_algorithm_internal_value).to eql(GZIP)

    @compression_options.default_algorithm = :deflate
    expect(@compression_options.default_algorithm_internal_value).to eql(DEFLATE)

    @compression_options.default_algorithm = :identity
    expect(@compression_options.default_algorithm_internal_value).to eql(IDENTITY)
  end

  it 'should be able to set the default level' do
    @compression_options.default_level = :none
    expect(@compression_options.default_level_internal_value).to eql(0)

    @compression_options.default_level = :low
    expect(@compression_options.default_level_internal_value).to eql(1)

    @compression_options.default_level = :medium
    expect(@compression_options.default_level_internal_value).to eql(2)

    @compression_options.default_level = :high
    expect(@compression_options.default_level_internal_value).to eql(3)
  end

  describe 'new' do
    it 'doesnt throw an error and initializes wrapped value' do
      expect { GRPC::Core::CompressionOptions.new }.to_not raise_error
    end

    it 'starts out with everything enabled' do
      expect(@compression_options.enabled_algorithms_bitset).to(
        eql(GZIP_BIT | DEFLATE_BIT | IDENTITY_BIT))
    end

    it 'starts out with no default levels or algorithms' do
      expect(@compression_options.default_algorithm_internal_value).to be_nil
      expect(@compression_options.default_level_internal_value).to be_nil
    end
  end

  describe 'sanity test' do
    GRPC_COMPRESSION_CHANNEL_DEFAULT_ALGORITHM_KEY = "grpc.default_compression_algorithm"
    GRPC_COMPRESSION_CHANNEL_DEFAULT_LEVEL_KEY = "grpc.default_compression_level"
    GRPC_COMPRESSION_CHANNEL_ENABLED_ALGORITHMS_BITSET_KEY = "grpc.compression_enabled_algorithms_bitset"

    it 'gives the correct channel args when nothing has been adjusted yet' do
      expect(@compression_options.channel_args_for_settings).to(
        eql({ 'grpc.compression_enabled_algorithms_bitset' => 0x7 }))
    end

    it 'gives the correct channel args after everything has been disabled' do
      @compression_options.default_algorithm = :identity
      @compression_options.default_level = :none
      @compression_options.disable_algorithms(:gzip, :deflate)

      expect(@compression_options.channel_args_for_settings).to(
        eql({ 'grpc.default_compression_algorithm' => 0,
              'grpc.default_compression_level' => 0,
              'grpc.compression_enabled_algorithms_bitset' => 0x1 }))
    end

    it 'gives the correct channel args after all settings have been adjusted lightly' do
      @compression_options.default_algorithm = :deflate
      @compression_options.default_level = :medium
      @compression_options.disable_algorithms(:gzip)

      expect(@compression_options.channel_args_for_settings).to(
       eql({ 'grpc.default_compression_algorithm' => 1,
             'grpc.default_compression_level' => 2,
             'grpc.compression_enabled_algorithms_bitset' => 0x3 }))
    end

    it 'gives the correct channel args after all settings have been adjusted lightly' do
      @compression_options.default_algorithm = :gzip
      @compression_options.default_level = :low
      @compression_options.disable_algorithms(:deflate)

      expect(@compression_options.channel_args_for_settings).to(
        eql({ 'grpc.default_compression_algorithm' => 2,
              'grpc.default_compression_level' => 1,
              'grpc.compression_enabled_algorithms_bitset' => 0x5 }))
    end

    it 'gives the correct channel args after all settings have been adjusted multiple times' do
      @compression_options.default_algorithm = :gzip
      @compression_options.default_level = :medium

      @compression_options.disable_algorithms(:deflate)
      @compression_options.enable_algorithms(:gzip, :deflate)

      @compression_options.default_algorithm = :deflate
      @compression_options.default_level = :high

      @compression_options.disable_algorithms(:gzip, :deflate)
      @compression_options.enable_algorithms(:gzip, :deflate)

      expect(@compression_options.channel_args_for_settings).to(
        eql({ 'grpc.default_compression_algorithm' => 1,
              'grpc.default_compression_level' => 3,
              'grpc.compression_enabled_algorithms_bitset' => 0x7 }))
    end
  end
end
