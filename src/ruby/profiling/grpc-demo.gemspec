# encoding: utf-8

Gem::Specification.new do |s|
  s.name          = 'grpc-demo'
  s.version       = '0.11.0'
  s.authors       = ['gRPC Authors']
  s.email         = 'temiola@google.com'
  s.homepage      = 'https://github.com/grpc/grpc'
  s.summary       = 'gRPC Ruby overview sample'
  s.description   = 'Simple demo of using gRPC from Ruby'

  s.files         = `git ls-files -- ruby/*`.split("\n")
  s.executables   = `git ls-files -- greeter*.rb`.split("\n").map do |f|
    File.basename(f)
  end
  s.platform      = Gem::Platform::RUBY

  s.add_dependency 'ruby-prof'
  s.add_dependency 'google-protobuf', '~> 3.0.0.alpha.5.0.3'

  s.add_development_dependency 'bundler', '~> 1.7'
end
