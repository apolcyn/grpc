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
  ALGORITHMS = {
    identity: 0,
    deflate: 1,
    gzip: 2
  }

  ALGORITHM_BITS = {
    identity: 0x1,
    deflate: 0x2,
    gzip: 0x4,
  }

  ALL_ENABLED_BITSET = 0x7

  COMPRESS_LEVELS = {
    none: 0,
    low: 1,
    medium: 2,
    high: 3
  }

  before(:example) do
    @compression_options = GRPC::Core::CompressionOptions.new
  end

  it 'implements to_s' do
    expect(@compression_options.to_s).to_not raise_error
  end

  describe '#disable_algorithms' do
    ALGORITHMS.each_pair do |name, internal_value|
      it 'passes #{internal_value} to #disable_algorithm_internal for #{name}' do
          expect(@compression_options).to receive(:disable_algorithm_internal)
            .with(internal_value)
        @compression_options.disable_algorithms(name)
      end
    end

    it 'should work with multiple parameters' do
      ALGORITHMS.each_pair do |name, internal_value|
        expect(@compression_options).to receive(name)
          .with(internal_value)
      end
      @compression_options.disable_algorithms(ALGORITHMS.keys)
    end
  end

  describe '#new' do
    it 'should start out with all algorithms enabled' do
      expect(@compression_options.enabled_algorithms_bitset).to eql(ALL_ENABLED_BITSET)
    end

    it 'should start out with no default algorithm' do
      expect(@compression_options.default_algorithm).to be_nil
      expect(@compression_options.default_algorithm_internal_value).to be_nil
    end

    it 'should start out with no default level' do
      expect(@compression_options.default_level).to be_nil
      expect(@compression_options.default_level_internal_value).to be_nil
    end
  end

  describe '#enabled_algoritms_bitset' do
    it 'should respond to disabling one algorithm' do
      @compression_options.disable_algorithms(:gzip)
      expect(@compression_options.enabled_algorithms_bitset & ALGORHTM_BITS[:gzip]).to be_zero
    end

    it 'should respond to disabling multiple algorithms' do
      compression_algorithms = ALGORITHMS.keys.drop(:identity)
      @compression_options.disable_algorithms(compression_algorithms)
      expect(@compression_options.enabled_algorithms_bitset).to eql(ALGORITHM_BITS[:identity])
    end

    it 'should respond to disabling algorithms in sequence' do
      @compression_options.disable_algorithms(ALGORITHMS[:gzip])
      expect(@compression_options.enabled_algorithms_bitset & ALGORITHM_BITS[:gzip]).to be_zero

      @compression_options.disable_algorithms(ALGORITHMS[:deflate])
      expect(@compression_options.enabled_algorithms_bitset & ALGORITHM_BITS[:deflate]).to be_zero
    end
  end

  describe 'setting the default algorithm' do
    it 'should effect the readable name and internal value' do
      ALGORITHMS.keys.each_pair do |name, internal_value|
        @compression_options.default_algorithm = name
        expect(@compresion_options.default_algorithm).to eql(name)
        expect(@compresion_options.default_algorithm_internal_value).to eql(internal_value)
      end
    end

    it 'should fail with invalid algorithm names' do
      [:none, :low, :huffman, :unkown].each do |name|
        expect { @compression_options.default_algorithm = name }.to raise_error
      end
    end
  end

  describe 'setting the default level' do
    it 'should effect the readable name and internal value' do
      COMPRESS_LEVELS.each_pair do |level, internal_value|
        @compression_options.default_level = level
        expect(@compression_options.default_level).to eql(level)
        expect(@compression_options.default_level_internal_value).to eql(internal_value)
      end
    end

    it 'should fail with invalid level names' do
      [:identity, :gzip, :unkown, :any].each do |name|
        expect { @compression_options.default_level }.to raise_error
      end
    end
  end

  describe 'changing the settings and reading the final keys' do
    it 'gives the correct channel args when nothing has been adjusted yet' do
      expect(@compression_options.to_hash).to(
        eql('grpc.compression_enabled_algorithms_bitset' => 0x7))
    end

    it 'gives the correct channel args after everything has been disabled' do
      @compression_options.default_algorithm = :identity
      @compression_options.default_level = :none
      @compression_options.disable_algorithms(:gzip, :deflate)

      expect(@compression_options.to_hash).to(
        eql('grpc.default_compression_algorithm' => 0,
            'grpc.default_compression_level' => 0,
            'grpc.compression_enabled_algorithms_bitset' => 0x1))
    end

    it 'gives correct channel args after settings have been adjusted lightly' do
      @compression_options.default_algorithm = :gzip
      @compression_options.default_level = :low
      @compression_options.disable_algorithms(:deflate)

      expected_bitset = ALL_ENABLED_BITSET & (ALGORITHM_BITS[:deflate])

      expect(@compression_options.to_hash).to(
        eql('grpc.default_compression_algorithm' => ALGORITHMS[:gzip],
            'grpc.default_compression_level' => COMPRESS_LEVELS[:low],
            'grpc.compression_enabled_algorithms_bitset' => expected_bitset))
    end

    it 'gives correct channel args after settings adjusted multiple times' do
      @compression_options.default_algorithm = :gzip
      @compression_options.default_level = :medium
      @compression_options.disable_algorithms(:deflate)

      @compression_options.default_algorithm = :identity
      @compression_options.default_level = :high
      @compression_options.disable_algorithms(:gzip, :deflate)

      expected_bitset = ALL_ENABLED_BITSET
      expected_bitset &= ~(ALGORITHM_BITS[:gzip] | ALGORITHM_BITS[:deflate])

      expect(@compression_options.to_hash).to(
        eql('grpc.default_compression_algorithm' => ALGORITHMS[:identity],
            'grpc.default_compression_level' => COMPRESS_LEVELS[:high],
            'grpc.compression_enabled_algorithms_bitset' => expected_bitset))
    end
  end
end
