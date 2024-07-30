package main

import (
	"bufio"
	"fmt"
	"log"
	"os"
	"strings"

	alsa "github.com/cocoonlife/goalsa"
	opus "github.com/reluekiss/voto"
)

/* cmd := exec.Command("arecord", "--dump-hw-params")

   // Execute the command and capture the output
   output, err := cmd.CombinedOutput()
   if err != nil {
       fmt.Println("Error executing command:", err)
       return
   }

   // Print the output
   fmt.Println(string(output)) */
/* // Check hardware format capabilities
alsa.FormatS24LE
switch
	case :
	8, 16, 24, 32, 64
// Check if x86
if {
	format = alsa.FormatSXXLE
} else {
	format = alsa.FormatSXXBE
} */
func captureaudio() []int16 {
    c, err := alsa.NewCaptureDevice("default", 2, alsa.FormatS16LE, 48000, alsa.BufferParams{})
    if err != nil {
	log.Fatal(err)
    }
    defer c.Close()

    err = c.StartReadThread()
    if err != nil {
	log.Fatal(err)
    }
    
    enc, err := opus.NewOpusEncoder(48000, 2, 20, 2)
    if err != nil {
        log.Fatal(err)
    }
    defer enc.Close()

    var samples, returnBuffer []int16
    buffer := make([]byte, 2048)
    r := bufio.NewReader(os.Stdin)
    
    for {
	input, _ := r.ReadString('\n')
        input = strings.TrimSpace(input)

        if string(input[0]) == "q" {
            break
        }

	_, err = c.Read(samples)
    	if err != nil {
	    fmt.Println("Error reading from capture device:", err)
	    break
	}

	_, err := enc.EncodeSamples(samples, len(samples), buffer)
        if err != nil {
            log.Fatal(err)
        }
	returnBuffer = append(returnBuffer, samples...)
    }
    return returnBuffer
}

func playaudio(recordBuffer []int16) {
    var length int
    
    p, err := alsa.NewPlaybackDevice("default", 2, alsa.FormatS16LE, 48000, alsa.BufferParams{})
    if err != nil {
	log.Fatal(err)
    }
    defer p.Close()
    
    dec, err := opus.NewOpusDecoder(48000, 2, 20)
    if err != nil {
        log.Fatal(err)
    }
    defer dec.Close()
    
   var buffer []byte 
   for {
   	_, err := dec.Decode(buffer[:length])
   	if err != nil {
   	    log.Fatal(err)
   	}
	p.Write(buffer)
	if err != nil {
    	    log.Fatal(err)
    	}
   } 
}

func main() {
    var samples []int16
    r := bufio.NewReader(os.Stdin)
    
    for {
	input, _ := r.ReadString('\n')
        input = strings.TrimSpace(input)

        if string(input[0]) == "w" {
	    samples = captureaudio()
        }
    	playaudio(samples)
    }
}
