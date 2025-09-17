require './t/case'

with_fixture "readiness1/notification-fd" => "3\n",
             "readiness2/notification-fd" => "3",
             "readiness3/notification-fd" => "bad\n",
             "readiness1/run!" => <<EOF,
#!/bin/sh
# no readiness
exec sleep 100
EOF
             "readiness2/run!" => <<EOF,
#!/bin/sh
printf up >/dev/fd/3  # no newline
exec sleep 100
EOF
             "readiness3/run!" => <<EOF do |svdir|
#!/bin/sh
exec sleep 100
EOF
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "readiness1"])
    sleep 3
    `nitroctl` =~ /STARTING readiness1/  or raise
    `nitroctl` =~ /STARTING readiness2/  or raise
    `nitroctl` =~ /STARTING readiness3/  or raise
  }
end
