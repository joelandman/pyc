     program integral_sin
      implicit none
      real*8 a, b, h, x, sum, integral
      integer n, i
      
      a = 0.0d0
      b = 2.0d0 * 3.14159265358979d0
      n = 1000
      h = (b - a) / dble(n)
      
      sum = 0.0d0
      
      do i = 1, n-1
         x = a + dble(i) * h
         sum = sum + dsin(x)
      end do
      
      integral = h * (0.5d0 * dsin(a) + sum + 0.5d0 * dsin(b))
      
      print *, 'Integral of sin(x) from 0 to 2pi = ', integral
      print *, 'Expected value = 0.0'
      
      stop
      end
