require './t/case'

with_fixture "readiness/run!" => <<EOF_B, "readiness/notification-fd" => "0\n",
#!/bin/sh
nitroctl ready .
exec sleep 100
EOF_B
             "not_ready/run!" => <<EOF_B, "not_ready/notification-fd" => "0\n" do |svdir|
#!/bin/sh
exec sleep 100
EOF_B
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "readiness"])
    sleep 0.5
    `nitroctl` =~ /UP readiness/  or raise "nitroctl ready failed"
    sleep 3
    `nitroctl` =~ /STARTING not_ready/  or raise "notification_fd == 0 failed"
  }
end
