require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_b/run!" => <<EOF_B do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec sleep 100
EOF_B
  testcase(svdir) { |events|
    # service is brought up by default
    events.poll_for(["STARTING", "sv_a"])
    `nitroctl wait-up sv_a`
    $? == 0  or raise "wait-up failed"

    pid = spawn("nitroctl", "wait-down", "sv_a", "sv_b")
    sleep 0.5
    Process.wait(pid, Process::WNOHANG) == nil  or "wait-down exited too soon"
    
    `nitroctl down sv_a sv_b`
    Process.wait(pid)
    $? == 0  or raise "wait-down failed"

    pid = spawn("nitroctl", "wait-up", "sv_a", "sv_b")
    sleep 0.5
    Process.wait(pid, Process::WNOHANG) == nil  or "wait-up exited too soon"
    
    `nitroctl restart sv_a`
    `nitroctl restart sv_b`
    Process.wait(pid)
    $? == 0  or raise "wait-up failed"
  }
end
