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

describe GRPC::Core::CompressionAlgorithms do
  before(:each) do
    @known_algorithms = {
      NONE: 0,
      DEFLATE: 1,
      GZIP: 2,
    }
  end

  it 'should have constants for all known compression algorithms' do
    m = GRPC::Core::CompressionAlgorithms
    syms_and_codes = m.constants.collect { |c| [c, m.const_get(c)] }
    expect(Hash[syms_and_codes]).to eq(@known_algorithms)
  end
end

describe GRPC::Core::CompressionLevels do
  before(:each) do
    @known_levels = {
      NONE: 0,
      LOW: 1,
      MEDIUM: 2,
      HIGH: 3,
    }
  end

  it 'should have constants for all known compression levels' do
    m = GRPC::Core::CompressionLevels
    syms_and_codes = m.constants.collect { |c| [c, m.const_get(c)] }
    expect(Hash[syms_and_codes]).to eq(@known_algorithms)
  end
end

describe GRPC::Core::CompressionOptions do
  before(:example) do
    @compression_options = GRPC::Core::CompressionOptions.new
  end

  it 'should provide the correct bit set for the enabled algorithms' do
    GZIP = 0x2
    DEFLATE = 0x1
    NONE = 0x1

    @compression_options.enable_algorithms(:gzip, :deflate)
    expect(@compression_options.enabled_algorithms_bitset).to eql(GZIP | DEFLATE | NONE)

    @compression_options.disable_algorithms(:gzip)
    expect(@compression_options.enabled_algorithms_bitset).to eql(GZIP | NONE)
    
    @compression_options.disable_algorithms(:deflate)
    expect(@compression_options.enabled_algorithms_bitset).to eql(NONE)
    
    @compression_options.enable_algorithms(:gzip, :deflate)
    expect(@compression_options.enabled_algorithms_bitset).to eql(GZIP | DEFLATE | NONE)
  end

  it 'should be able to set the default algorithm' do
	  [:gzip => 0, :deflate => 1, :identity => 2].each_pair do |algorithm_name, internal_value|
	  @compression_options.default_algorithm = algorithm_name
	  expect(@compression_options.default_algorithm_internal_value).to eql(internal_value)
	  end
  end

  it 'should be able to set the default level' do
	  [:none => 0, :low => 1, :medium => 2, :high => 3].each_pair do |name, internal_value|
	  @compression_options.default_level = level_name
	  expect(@compression_options.default_level_internal_value).to eql(internal_value)
	  end
  end

  describe '#new' do
    it 'doesnt throw an error and initializes wrapped value' do
      expect { GRPC::Core::CompressionOptions.new }.to_not raise_error
    end

    it 'starts out with no compression enabled' do
	    expect(@compression_options.enabled_algorithms_bitset).to eql(0x1)
	    expect(@compression_options.default_algorithm_internal_value).to eql(0)
	    expect(@compression_options.default_level_internal_value).to eql(1)
    end
  end

  describe '#enable_algorithms' do
    it 'works with zero parameters' do
      expect { @compression_options.enable_algorithms }.to_not raise_error
    end

    it 'works with strings or symbols' do
      algorithm = GRPC::Core::CompressionOptions::COMPRESSION_ALGORITHMS.keys.last

      expect { @compression_options.disable_algorithms(algorithm) }.to_not raise_error
      expect { @compression_options.disable_algorithms(algorithm.downcase) }.to_not raise_error
      expect { @compression_options.disable_algorithms(algorithm.to_s) }.to_not raise_error
    end

    it 'works with multiple parameters' do
      algorithms = GRPC::Core::CompressionAlgorithms::COMPRESSION_ALGORITHMS.keys
      expect { @compression_options.disable_algorithms(algorithms) }.to_not raise_error
    end
  end

  #TODO make these examples shared
  describe '#disable_algorithms' do
    it 'works with zero parameters' do
      expect { @compression_options.enable_algorithms }.to_not raise_error
    end

    it 'works with string or symbols' do
      algorithm = GRPC::Core::CompressionOptions::COMPRESSION_ALGORITHMS.keys.last
      expect { @compression_options.disable_algorithms(algorithm.downcase) }.to_not raise_error
      expect { @compression_options.disable_algorithms(algorithm.to_s) }.to_not raise_error
    end

    it 'works with multiple parameters' do
      algorithms = GRPC::Core::CompressionAlgorithms::COMPRESSION_ALGORITHMS.keys
      expect { @compression_options.disable_algorithms(algorithms) }.to_not raise_error
    end
  end

  describe '#default_algorithm= and #default_algorithm' do
    it 'can be passed a string or a symbol' do
      algorithm = GRPC::Core::CompressionOptions::COMPRESSION_ALGORITHMS.keys.last
      expect { @compression_options.default_algorithm = algorithm }.to_not raise_error
      expect(@compression_options.default_algorithm).to eql(algorithm)
    end
  end

  describe '#default_level= and #default_level' do
    it 'can be assigned a known level' do
      level = GRPC::Core::CompressionLevels.constants.last
      expect { @compression_options.default_level = level }.to_not raise_error
      expect(@compression_options.default_level).to eql(level)
    end
  end
end
