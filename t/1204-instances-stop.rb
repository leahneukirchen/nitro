require './t/case'

with_fixture "sv@/run!" => <<EOF_A, "sv@a=" => "sv@" do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "sv@a"])

    `nitroctl stop sv@b`
    status = $?.exitstatus

    `nitroctl` =~ /sv@b/  and raise "stop created service"
    status != 0  or raise "stop succeeded for non existing service"

    `nitroctl pidof sv@c`
    $?.exitstatus != 0  or raise "pidof created service"
  }
end
