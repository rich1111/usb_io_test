package main
import (
	"time"
	"github.com/google/gousb"
)
func main() {
	var ep *gousb.InEndpoint
	ep.Timeout = 50 * time.Millisecond
}
