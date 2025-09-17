require './t/case'

with_fixture "sv@/run!" => <<EOF_A, "sv@a=" => "sv@" do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "sv@a"])

    `nitroctl up sv@`
    status = $?.exitstatus
    
    `nitroctl` =~ /STARTING sv@ /  and raise "invalid service name is up"
    status != 0  or raise "nitroctl up should have failed"
  }
end

