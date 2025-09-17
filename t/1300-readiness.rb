require './t/case'

with_fixture "plain/run!" => <<EOF_A, "readiness/run!" => <<EOF_B, "readiness/notification-fd" => "37\n",
#!/bin/sh
sleep 100
EOF_A
#!/bin/sh
ruby -e 'exit IO.open(37).stat.pipe?'
echo $? >checkfd
echo up >/dev/fd/37
sleep 100
EOF_B
              "sv@/run!" => <<EOF_C, "sv@/notification-fd" => "3\n", "sv@abc=" => "sv@" do |svdir|
#!/bin/sh
echo up >/dev/fd/3
ruby -e 'exit IO.open(3).stat.pipe?'
echo $? >checkfd
sleep 100
EOF_C
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "plain"])
    sleep 0.1
    `nitroctl` =~ /UP plain/  and raise "default timeout failed"
    `nitroctl` =~ /UP readiness/  or raise "readiness failed"

    File.read(File.join(svdir, "readiness/checkfd")) == "0\n"  or raise "no pipe created"
    File.read(File.join(svdir, "sv@/checkfd")) == "0\n"  or raise "no pipe created in instance"

    events.poll_for(["UP", "plain"])
    `nitroctl` =~ /UP plain/  or raise "default timeout failed"
  }
end
