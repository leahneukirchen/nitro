require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/log=" => "../mylog",
#!/bin/sh
echo A
sleep 1
echo A
echo A
sleep 100
EOF_A
             "sv_b/run!" => <<EOF_B, "sv_b/log=" => "../mylog",
#!/bin/sh
echo B
echo B
sleep 1
echo B
sleep 100
EOF_B
             "mylog/run!" => <<EOF do |svdir|
#!/bin/sh
exec cat >mylog.txt
EOF
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
    events.poll_for(["UP", "sv_b"])

    File.read(File.join(svdir, "mylog/mylog.txt")).lines(:chomp => true).
      sort == ["A","A","A","B","B","B"]  or raise "wrong output"
  }
end
