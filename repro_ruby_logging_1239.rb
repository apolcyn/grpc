#!/usr/bin/ruby
#
# Summary description of library or script.
#
# This doc string should contain an overall description of the module/script
# and can optionally briefly describe exported classes and functions.
#
#    ClassFoo:      One line summary.
#    function_foo:  One line summary.
#
# $Id$

require 'grpc'
require 'google-apis'
require 'google/logging/v2/logging_pb'
require 'google/logging/v2/logging_services_pb'
require 'google/logging/v2/log_entry_pb'
require 'googleauth'

payload = 'testing partial success'
resource = Google::Api::MonitoredResource.new(
	  type: 'gce_instance',
	    labels: {
		      'zone' => 'us-central1-b',
		          'instance_id' => ''
	    }
)
good_log_name = 'projects/test-project/logs/good'
bad_log_name = 'projects/test-project/logs/b:ad'

entries = []
entry1 = Google::Logging::V2::LogEntry.new(
	  resource: resource,
	          text_payload: payload,
		    log_name: bad_log_name
)
entry2 = Google::Logging::V2::LogEntry.new(
	  resource: resource,
	          text_payload: payload,
		    log_name: bad_log_name
)
entry3 = Google::Logging::V2::LogEntry.new(
	  resource: resource,
	          text_payload: payload,
		    log_name: good_log_name
)

entries.push(entry1)
entries.push(entry2)
entries.push(entry3)

puts "Entries to write:\n= #{entries.inspect}\n\n"

ssl_creds = GRPC::Core::ChannelCredentials.new
authentication = Google::Auth.get_application_default
creds = GRPC::Core::CallCredentials.new(authentication.updater_proc)
creds = ssl_creds.compose(creds)
client = Google::Logging::V2::LoggingServiceV2::Stub.new(
	  'logging.googleapis.com', creds)

begin
	  write_request = Google::Logging::V2::WriteLogEntriesRequest.new(
		      log_name: 'projects/grpc-testing/test_logs/normal',
		          resource: resource,
			      entries: entries,
			          partial_success: true
				    )
	    response = client.write_log_entries(write_request)
rescue => error
	  puts "error:\n#{error.inspect}\n"
	    puts "error metadata =\n#{error.metadata.inspect}\n"
end
